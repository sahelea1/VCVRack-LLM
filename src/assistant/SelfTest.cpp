#include <assistant/SelfTest.hpp>
#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

#include <GLFW/glfw3.h>

#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <asset.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <logger.hpp>
#include <patch.hpp>
#include <plugin.hpp>
#include <string.hpp>
#include <system.hpp>
#include <window/Window.hpp>


namespace rack {
namespace assistant {


using namespace helpers;


namespace {


// ---- Result recording ----------------------------------------------------------------

struct SelfTestRun {
	int passed = 0;
	int failed = 0;
	std::vector<std::string> failures;

	void check(const std::string& name, bool ok, const std::string& detail = "") {
		if (ok) {
			passed++;
			INFO("[assistant selftest] PASS %s", name.c_str());
		}
		else {
			failed++;
			std::string msg = detail.empty() ? name : (name + ": " + detail);
			failures.push_back(msg);
			WARN("[assistant selftest] FAIL %s", msg.c_str());
		}
	}
};


// ---- JSON helpers --------------------------------------------------------------------

json_t* jget(json_t* o, const char* k) {
	return (o && json_is_object(o)) ? json_object_get(o, k) : NULL;
}

std::string jstr(json_t* o, const char* k) {
	json_t* v = jget(o, k);
	return (v && json_is_string(v)) ? json_string_value(v) : "";
}

int64_t jint(json_t* o, const char* k, int64_t def = -999999) {
	json_t* v = jget(o, k);
	return (v && json_is_integer(v)) ? (int64_t) json_integer_value(v) : def;
}

double jnum(json_t* o, const char* k, double def = NAN) {
	json_t* v = jget(o, k);
	return (v && (json_is_real(v) || json_is_integer(v))) ? json_number_value(v) : def;
}

bool jbool(json_t* o, const char* k, bool def = false) {
	json_t* v = jget(o, k);
	return (v && json_is_boolean(v)) ? json_is_true(v) : def;
}

size_t jlen(json_t* arr) {
	return (arr && json_is_array(arr)) ? json_array_size(arr) : 0;
}

bool has(const std::string& s, const std::string& sub) {
	return s.find(sub) != std::string::npos;
}

/** Finds the entry with the given name in an array of {id, name, ...} objects. Returns its id or -1. */
int64_t idByName(json_t* arr, const std::string& name) {
	size_t i;
	json_t* e;
	json_array_foreach(arr, i, e) {
		if (lowercase(jstr(e, "name")) == lowercase(name))
			return jint(e, "id", -1);
	}
	return -1;
}

json_t* entryByName(json_t* arr, const std::string& name) {
	size_t i;
	json_t* e;
	json_array_foreach(arr, i, e) {
		if (lowercase(jstr(e, "name")) == lowercase(name))
			return e;
	}
	return NULL;
}


struct Resp {
	ToolResult r;
	JsonPtr j;
	bool ok() const {
		return r.ok && j && jbool(j.get(), "ok");
	}
	std::string error() const {
		return jstr(j.get(), "error");
	}
	json_t* get(const char* k) const {
		return jget(j.get(), k);
	}
};


struct Scenario {
	SelfTestRun& t;
	ToolRegistry& reg;
	ToolContext* ctx;

	Scenario(SelfTestRun& t) : t(t), reg(defaultRegistry()), ctx(NULL) {}

	Resp call(const std::string& tool, const std::string& args) {
		Resp resp;
		resp.r = reg.execute(tool, args, *ctx);
		json_error_t err;
		resp.j.reset(json_loads(resp.r.content.c_str(), 0, &err));
		bool valid = resp.j && json_is_object(resp.j.get()) && jget(resp.j.get(), "ok") && json_is_boolean(jget(resp.j.get(), "ok")) && (jbool(resp.j.get(), "ok") == resp.r.ok);
		if (!valid)
			t.check("result JSON well-formed for " + tool, false, resp.r.content);
		return resp;
	}
};


// ---- Patch inspection helpers --------------------------------------------------------

struct Snapshot {
	std::map<int64_t, std::string> modules;                // id -> "plugin/model@hp,row"
	std::map<std::pair<int64_t, int>, float> params;       // (module id, param id) -> value
	std::set<std::string> cables;                          // "id:outMod.outId>inMod.inId"
};

Snapshot takeSnapshot() {
	Snapshot s;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		engine::Module* m = mw->module;
		GridPos p = gridPosOf(mw);
		s.modules[m->id] = string::f("%s/%s@%d,%d", m->model->plugin->slug.c_str(), m->model->slug.c_str(), p.hp, p.row);
		for (size_t i = 0; i < m->params.size(); i++)
			s.params[std::make_pair(m->id, (int) i)] = m->params[i].getValue();
	}
	for (app::CableWidget* cw : APP->scene->rack->getCompleteCables()) {
		if (!cw->cable)
			continue;
		s.cables.insert(string::f("%lld:%lld.%d>%lld.%d", (long long) cw->cable->id, (long long) cw->cable->outputModule->id, cw->cable->outputId, (long long) cw->cable->inputModule->id, cw->cable->inputId));
	}
	return s;
}

/** Returns "" if equal, else a description of the first difference. */
std::string diffSnapshots(const Snapshot& a, const Snapshot& b) {
	if (a.modules.size() != b.modules.size())
		return string::f("module count %d vs %d", (int) a.modules.size(), (int) b.modules.size());
	for (const auto& kv : a.modules) {
		auto it = b.modules.find(kv.first);
		if (it == b.modules.end())
			return string::f("module %lld missing", (long long) kv.first);
		if (it->second != kv.second)
			return string::f("module %lld differs: %s vs %s", (long long) kv.first, kv.second.c_str(), it->second.c_str());
	}
	if (a.cables != b.cables)
		return string::f("cables differ (%d vs %d)", (int) a.cables.size(), (int) b.cables.size());
	if (a.params.size() != b.params.size())
		return "param count differs";
	for (const auto& kv : a.params) {
		auto it = b.params.find(kv.first);
		if (it == b.params.end() || it->second != kv.second)
			return string::f("param %lld/%d differs: %g vs %g", (long long) kv.first.first, kv.first.second, kv.second, it == b.params.end() ? NAN : it->second);
	}
	return "";
}

size_t moduleCount() {
	return APP->scene->rack->getModules().size();
}

size_t cableCount() {
	return APP->scene->rack->getCompleteCables().size();
}

/** Returns "" if no two modules overlap, else a description. */
std::string findOverlap() {
	std::vector<app::ModuleWidget*> mws = APP->scene->rack->getModules();
	for (size_t i = 0; i < mws.size(); i++) {
		for (size_t j = i + 1; j < mws.size(); j++) {
			math::Rect a = mws[i]->getGridBox();
			math::Rect b = mws[j]->getGridBox();
			if (a.intersects(b) || mws[i]->box.intersects(mws[j]->box)) {
				return string::f("%s (id %lld) overlaps %s (id %lld)", mws[i]->model->slug.c_str(), (long long) mws[i]->module->id, mws[j]->model->slug.c_str(), (long long) mws[j]->module->id);
			}
		}
	}
	return "";
}


// ---- The tools scenario --------------------------------------------------------------

void toolsScenario(SelfTestRun& t) {
	Scenario S(t);
	ToolRegistry& reg = S.reg;
	app::RackWidget* rack = APP->scene->rack;
	const std::string originalPatchPath = APP->patch->path;

	// ---- Registry --------------------------------------------------------------------
	{
		const char* names[] = {"get_patch", "add_module", "remove_module", "move_module", "set_param", "connect", "disconnect", "save_patch", "clear_patch", "search_modules", "get_module_info"};
		bool all = true;
		std::string missing;
		for (const char* n : names) {
			if (!reg.find(n)) {
				all = false;
				missing += std::string(n) + " ";
			}
		}
		t.check("registry has all tools", all, "missing: " + missing);
		JsonPtr arr;
		{
			json_error_t e;
			arr.reset(json_loads(reg.toolsJson().c_str(), 0, &e));
		}
		bool shapeOk = arr && json_is_array(arr.get()) && jlen(arr.get()) == reg.all().size();
		size_t i;
		json_t* it;
		if (shapeOk) {
			json_array_foreach(arr.get(), i, it) {
				json_t* fn = jget(it, "function");
				if (jstr(it, "type") != "function" || !fn || jstr(fn, "name").empty() || jstr(fn, "description").empty() || jstr(jget(fn, "parameters"), "type") != "object")
					shapeOk = false;
			}
		}
		t.check("toolsJson is an OpenAI tools array", shapeOk);
	}

	// ---- Phase A: clear the loaded patch in its own undo action ------------------------
	if (moduleCount() == 0) {
		// Make sure the clear has something to undo
		history::ComplexAction* pre = new history::ComplexAction;
		pre->name = "selftest prepare";
		ToolContext pctx;
		pctx.undo = pre;
		S.ctx = &pctx;
		Resp r = S.call("add_module", R"({"plugin":"Core","model":"Blank"})");
		t.check("prepare: add Core Blank", r.ok(), r.error());
		APP->history->push(pre);
	}
	const size_t originalModules = moduleCount();
	const Snapshot originalSnapshot = takeSnapshot();
	size_t originalCables = cableCount();

	{
		history::ComplexAction* a0 = new history::ComplexAction;
		a0->name = "selftest clear";
		ToolContext c0;
		c0.undo = a0;
		S.ctx = &c0;

		std::string conf = reg.confirmationFor("clear_patch", "{}");
		t.check("clear_patch confirmation non-empty when modules exist", originalModules > 0 && has(conf, "Remove all") && has(conf, string::f("%d modules", (int) originalModules)), conf);
		t.check("clear_patch mentions cable count", has(conf, string::f("%d cables", (int) originalCables)), conf);

		Resp r = S.call("clear_patch", "{}");
		t.check("clear_patch ok", r.ok(), r.error());
		t.check("clear_patch removed all modules", moduleCount() == 0 && APP->engine->getNumModules() == 0);
		t.check("clear_patch removed all cables", cableCount() == 0 && APP->engine->getNumCables() == 0);
		t.check("clear_patch reports counts", jint(r.j.get(), "removed_modules") == (int64_t) originalModules && jint(r.j.get(), "removed_cables") == (int64_t) originalCables);
		t.check("clear_patch summary", has(r.r.summary, "Cleared patch") && r.r.mutated);
		t.check("clear_patch confirmation empty on empty patch", reg.confirmationFor("clear_patch", "{}").empty());
		Resp r2 = S.call("clear_patch", "{}");
		t.check("clear_patch on empty patch is ok and not mutating", r2.ok() && !r2.r.mutated);
		t.check("clear_patch recorded undo actions", !a0->isEmpty());
		if (!a0->isEmpty())
			APP->history->push(a0);
		else
			delete a0;
	}

	// ---- get_patch on an empty patch ---------------------------------------------------
	{
		ToolContext c;
		history::ComplexAction dummy;
		c.undo = &dummy;
		S.ctx = &c;
		Resp r = S.call("get_patch", "{}");
		t.check("get_patch empty ok", r.ok(), r.error());
		t.check("get_patch empty counts", jint(r.j.get(), "module_count") == 0 && jint(r.j.get(), "cable_count") == 0 && jlen(r.get("modules")) == 0 && jlen(r.get("cables")) == 0 && json_is_array(r.get("selected_module_ids")));
		t.check("get_patch is read-only", r.r.readOnly && !r.r.mutated);
		t.check("get_patch has unsaved flag", jget(r.j.get(), "unsaved") != NULL && json_is_boolean(jget(r.j.get(), "unsaved")));
		t.check("get_patch summary", has(r.r.summary, "Read patch"), r.r.summary);
	}

	// ---- Catalog: search_modules / get_module_info ---------------------------------------
	{
		ToolContext c;
		S.ctx = &c;
		Resp r = S.call("search_modules", R"({"query":"VCO"})");
		t.check("search_modules VCO ok", r.ok(), r.error());
		bool foundVco = false;
		size_t i;
		json_t* e;
		json_array_foreach(r.get("results"), i, e) {
			if (jstr(e, "plugin") == "Fundamental" && jstr(e, "model") == "VCO")
				foundVco = true;
		}
		t.check("search_modules VCO finds Fundamental VCO", foundVco);
		t.check("search_modules default limit", jlen(r.get("results")) >= 1 && jlen(r.get("results")) <= 12);
		t.check("search_modules total_matches >= results", jint(r.j.get(), "total_matches") >= (int64_t) jlen(r.get("results")));
		json_t* first = json_array_get(r.get("results"), 0);
		t.check("search_modules result fields", first && !jstr(first, "name").empty() && !jstr(first, "plugin").empty() && !jstr(first, "model").empty() && jget(first, "tags") && json_is_array(jget(first, "tags")) && jget(first, "description") && jget(first, "brand"));
		t.check("search_modules summary", has(r.r.summary, "Searched \"VCO\""), r.r.summary);
		t.check("search_modules read-only", r.r.readOnly);

		// Tag filter
		Resp rf = S.call("search_modules", R"({"query":"VCF","tag":"Filter"})");
		bool foundVcf = false, allFilter = rf.ok() && jlen(rf.get("results")) > 0;
		json_array_foreach(rf.get("results"), i, e) {
			if (jstr(e, "plugin") == "Fundamental" && jstr(e, "model") == "VCF")
				foundVcf = true;
			bool hasTag = false;
			size_t k;
			json_t* tg;
			json_array_foreach(jget(e, "tags"), k, tg) {
				if (json_is_string(tg) && std::string(json_string_value(tg)) == "Filter")
					hasTag = true;
			}
			if (!hasTag)
				allFilter = false;
		}
		t.check("search_modules tag Filter finds VCF", foundVcf, rf.error());
		t.check("search_modules tag filter only returns tagged modules", allFilter);
		Resp ro = S.call("search_modules", R"({"query":"VCO","tag":"Filter"})");
		bool vcoInFilter = false;
		json_array_foreach(ro.get("results"), i, e) {
			if (jstr(e, "plugin") == "Fundamental" && jstr(e, "model") == "VCO")
				vcoInFilter = true;
		}
		t.check("search_modules VCO + tag Filter excludes VCO", ro.ok() && !vcoInFilter, ro.error());
		Resp rt = S.call("search_modules", R"({"query":"","tag":"vcf"})");
		t.check("search_modules tag alias + empty query", rt.ok() && jlen(rt.get("results")) > 0, rt.error());
		Resp rbad = S.call("search_modules", R"({"query":"VCO","tag":"NoSuchTag"})");
		t.check("search_modules unknown tag -> error with valid tags", !rbad.ok() && has(rbad.error(), "Unknown tag") && has(rbad.error(), "Filter"), rbad.error());
		Resp rl = S.call("search_modules", R"({"query":"a","limit":3})");
		t.check("search_modules limit 3", rl.ok() && jlen(rl.get("results")) <= 3 && jlen(rl.get("results")) >= 1, rl.error());
		Resp rl0 = S.call("search_modules", R"({"query":"a","limit":0})");
		t.check("search_modules limit 0 clamps to 1", rl0.ok() && jlen(rl0.get("results")) == 1, rl0.error());
		Resp rlbig = S.call("search_modules", R"({"query":"","tag":"Utility","limit":100000})");
		t.check("search_modules limit capped at 40", rlbig.ok() && jlen(rlbig.get("results")) <= 40, rlbig.error());
		Resp rlbad = S.call("search_modules", R"({"query":"a","limit":"many"})");
		t.check("search_modules invalid limit -> error", !rlbad.ok());
		Resp rnone = S.call("search_modules", "{}");
		t.check("search_modules without query/tag -> error", !rnone.ok());
		Resp rzero = S.call("search_modules", R"({"query":"zzzzqqqqxxxx"})");
		t.check("search_modules no match -> ok with 0 results", rzero.ok() && jlen(rzero.get("results")) == 0);
		Resp rsub = S.call("search_modules", R"({"query":"ADSR"})");
		bool foundAdsr = false;
		json_array_foreach(rsub.get("results"), i, e) {
			if (jstr(e, "plugin") == "Fundamental" && jstr(e, "model") == "ADSR")
				foundAdsr = true;
		}
		t.check("search_modules finds ADSR", foundAdsr, rsub.error());

		// get_module_info by plugin/model
		Resp ri = S.call("get_module_info", R"({"plugin":"Fundamental","model":"VCF"})");
		t.check("get_module_info VCF ok", ri.ok(), ri.error());
		json_t* params = ri.get("params");
		t.check("get_module_info VCF has params", jlen(params) >= 4);
		bool cutoff = false;
		json_array_foreach(params, i, e) {
			std::string n = lowercase(jstr(e, "name"));
			if (has(n, "cutoff") || has(n, "frequency"))
				cutoff = true;
		}
		t.check("get_module_info VCF has a cutoff/frequency param", cutoff);
		json_t* p0 = json_array_get(params, 0);
		t.check("get_module_info param fields", p0 && jget(p0, "id") && jget(p0, "name") && jget(p0, "min") && jget(p0, "max") && jget(p0, "default") && jget(p0, "unit") && jget(p0, "snap") && jget(p0, "min_display") && jget(p0, "max_display") && jget(p0, "default_display"));
		t.check("get_module_info VCF ports", jlen(ri.get("inputs")) >= 1 && jlen(ri.get("outputs")) >= 1 && idByName(ri.get("inputs"), "Audio") >= 0 && idByName(ri.get("outputs"), "Lowpass filter") >= 0);
		t.check("get_module_info width and tags", jint(ri.j.get(), "width_hp") > 0 && json_is_array(ri.get("tags")) && jstr(ri.j.get(), "plugin") == "Fundamental" && jstr(ri.j.get(), "model") == "VCF");
		t.check("get_module_info model-level has no live values", p0 && !jget(p0, "value") && !jget(ri.j.get(), "module_id"));
		Resp ri2 = S.call("get_module_info", R"({"plugin":"Fundamental","model":"VCF"})");
		t.check("get_module_info is deterministic (cached)", ri2.ok() && ri2.r.content == ri.r.content);
		t.check("get_module_info summary", has(ri.r.summary, "Inspected"), ri.r.summary);

		Resp rvco = S.call("get_module_info", R"({"plugin":"Fundamental","model":"VCO"})");
		json_t* fm = entryByName(rvco.get("params"), "FM mode");
		t.check("get_module_info SwitchQuantity options", fm && jlen(jget(fm, "options")) == 2 && json_is_string(json_array_get(jget(fm, "options"), 1)) && std::string(json_string_value(json_array_get(jget(fm, "options"), 1))) == "Linear");
		Resp runk = S.call("get_module_info", R"({"plugin":"Fundamental","model":"DoesNotExist"})");
		t.check("get_module_info unknown model -> error", !runk.ok() && has(runk.error(), "search_modules"), runk.error());
		Resp runkp = S.call("get_module_info", R"({"plugin":"NoPlugin","model":"VCO"})");
		t.check("get_module_info unknown plugin -> error", !runkp.ok());
		Resp rmiss = S.call("get_module_info", "{}");
		t.check("get_module_info without args -> error", !rmiss.ok());
		Resp rnoid = S.call("get_module_info", R"({"module_id":123456789})");
		t.check("get_module_info unknown module id -> error", !rnoid.ok() && has(rnoid.error(), "get_patch"), rnoid.error());

		// Temporary-module introspection must not crash for any Core or Fundamental model
		int tried = 0, failedInfo = 0;
		std::string failedNames;
		for (plugin::Plugin* p : plugin::plugins) {
			if (p->slug != "Core" && p->slug != "Fundamental")
				continue;
			for (plugin::Model* m : p->models) {
				tried++;
				Resp rr = S.call("get_module_info", string::f("{\"plugin\":\"%s\",\"model\":\"%s\"}", p->slug.c_str(), m->slug.c_str()));
				if (!rr.ok()) {
					failedInfo++;
					failedNames += p->slug + "/" + m->slug + ": " + rr.error() + "; ";
				}
			}
		}
		t.check("get_module_info works for all Core and Fundamental models", tried > 0 && failedInfo == 0, failedNames);
		t.check("engine untouched by temporary modules", APP->engine->getNumModules() == 0);
	}

	// ---- Phase D: build a patch in a single undo action -----------------------------------
	history::ComplexAction* a1 = new history::ComplexAction;
	a1->name = "selftest build";
	ToolContext c1;
	c1.undo = a1;
	S.ctx = &c1;

	int64_t vcoId = -1, vcfId = -1, vcaId = -1, adsrId = -1, vca2Id = -1, lfoNearId = -1;
	{
		Resp r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCO"})");
		t.check("add_module VCO", r.ok(), r.error());
		vcoId = jint(r.j.get(), "module_id", -1);
		t.check("add_module result fields", vcoId >= 0 && jstr(r.j.get(), "plugin") == "Fundamental" && jstr(r.j.get(), "model") == "VCO" && !jstr(r.j.get(), "name").empty() && jint(jget(r.j.get(), "pos"), "hp") == 0 && jint(jget(r.j.get(), "pos"), "row") == 0 && jint(r.j.get(), "width_hp") > 0);
		t.check("add_module summary / mutated", has(r.r.summary, "added") && r.r.mutated, r.r.summary);
		t.check("add_module tracks added ids", c1.addedModuleIds.size() == 1 && c1.addedModuleIds[0] == vcoId);
		t.check("add_module counted as mutation", c1.mutationsSinceSave == 1);
		t.check("add_module is in the engine and rack", APP->engine->getModule(vcoId) != NULL && rack->getModule(vcoId) != NULL);

		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCF"})");
		t.check("add_module VCF", r.ok(), r.error());
		vcfId = jint(r.j.get(), "module_id", -1);
		app::ModuleWidget* vcoMw = rack->getModule(vcoId);
		t.check("add_module places next module to the right of the last one", vcoMw && jint(jget(r.j.get(), "pos"), "hp") == gridPosOf(vcoMw).hp + widthHp(vcoMw) && jint(jget(r.j.get(), "pos"), "row") == 0);

		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCA-1"})");
		t.check("add_module VCA", r.ok(), r.error());
		vcaId = jint(r.j.get(), "module_id", -1);
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"ADSR"})");
		t.check("add_module ADSR", r.ok(), r.error());
		adsrId = jint(r.j.get(), "module_id", -1);
		t.check("4 modules, no overlaps", moduleCount() == 4 && findOverlap().empty(), findOverlap());

		// Explicit occupied position -> note + free slot
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCA-1","position":{"hp":0,"row":0}})");
		t.check("add_module occupied position ok", r.ok(), r.error());
		vca2Id = jint(r.j.get(), "module_id", -1);
		t.check("add_module occupied position gives a note", !jstr(r.j.get(), "note").empty(), r.r.content);
		t.check("add_module occupied position -> different free slot, no overlap", jint(jget(r.j.get(), "pos"), "row") == 0 && jint(jget(r.j.get(), "pos"), "hp") != 0 && findOverlap().empty(), findOverlap());

		// Explicit free position
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"LFO","position":{"hp":3,"row":1}})");
		int64_t lfoRowId = jint(r.j.get(), "module_id", -1);
		(void) lfoRowId;
		t.check("add_module explicit free position is honored", r.ok() && jint(jget(r.j.get(), "pos"), "hp") == 3 && jint(jget(r.j.get(), "pos"), "row") == 1 && jstr(r.j.get(), "note").empty(), r.r.content);

		// near_module_id
		r = S.call("add_module", string::f("{\"plugin\":\"Fundamental\",\"model\":\"LFO\",\"near_module_id\":%lld}", (long long) vcoId));
		lfoNearId = jint(r.j.get(), "module_id", -1);
		t.check("add_module near_module_id ok, to the right of the anchor, no overlap", r.ok() && jint(jget(r.j.get(), "pos"), "hp") >= gridPosOf(vcoMw).hp + widthHp(vcoMw) && jint(jget(r.j.get(), "pos"), "row") == 0 && findOverlap().empty(), r.r.content + " " + findOverlap());

		// Errors
		size_t before = moduleCount();
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"NoSuchModel"})");
		t.check("add_module unknown model -> error suggesting search_modules", !r.ok() && has(r.error(), "search_modules"), r.error());
		r = S.call("add_module", R"({"plugin":"NoSuchPlugin","model":"VCO"})");
		t.check("add_module unknown plugin -> error", !r.ok() && has(r.error(), "search_modules"), r.error());
		r = S.call("add_module", R"({"plugin":5,"model":[]})");
		t.check("add_module wrong arg types -> error", !r.ok());
		r = S.call("add_module", "{}");
		t.check("add_module missing args -> error", !r.ok());
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCO","position":{"hp":"abc","row":0}})");
		t.check("add_module invalid position -> error", !r.ok());
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCO","position":{"hp":999999999,"row":0}})");
		t.check("add_module out-of-range position -> error", !r.ok());
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCO","near_module_id":99999999})");
		t.check("add_module invalid near_module_id -> error", !r.ok());
		t.check("failed add_module calls did not add modules", moduleCount() == before && APP->engine->getNumModules() == before);
	}

	// ---- Connect -----------------------------------------------------------------------
	int64_t cableSawToVcf = -1, cableVcfToVca = -1, cableAdsr = -1;
	Resp vcoInfo = S.call("get_module_info", string::f("{\"module_id\":%lld}", (long long) vcoId));
	Resp vcfInfo = S.call("get_module_info", string::f("{\"module_id\":%lld}", (long long) vcfId));
	Resp vcaInfo = S.call("get_module_info", string::f("{\"module_id\":%lld}", (long long) vcaId));
	Resp adsrInfo = S.call("get_module_info", string::f("{\"module_id\":%lld}", (long long) adsrId));
	int64_t sawOut = idByName(vcoInfo.get("outputs"), "Sawtooth");
	int64_t sqrOut = idByName(vcoInfo.get("outputs"), "Square");
	int64_t sineOut = idByName(vcoInfo.get("outputs"), "Sine");
	int64_t vcfAudioIn = idByName(vcfInfo.get("inputs"), "Audio");
	int64_t vcfFreqIn = idByName(vcfInfo.get("inputs"), "Frequency");
	{
		t.check("get_module_info instance has module_id, pos, values", vcoInfo.ok() && jint(vcoInfo.j.get(), "module_id") == vcoId && jget(vcoInfo.j.get(), "pos") && jget(json_array_get(vcoInfo.get("params"), 0), "value") && jget(json_array_get(vcoInfo.get("params"), 0), "display"), vcoInfo.error());
		t.check("get_module_info instance has connected flags", jget(json_array_get(vcfInfo.get("inputs"), 0), "connected") != NULL && !jbool(json_array_get(vcfInfo.get("inputs"), 0), "connected", true));
		t.check("port ids found by name", sawOut >= 0 && sqrOut >= 0 && sineOut >= 0 && vcfAudioIn >= 0 && vcfFreqIn >= 0);

		Resp r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":%lld,\"to_module\":%lld,\"input_id\":%lld}", (long long) vcoId, (long long) sawOut, (long long) vcfId, (long long) vcfAudioIn));
		t.check("connect by index", r.ok(), r.error());
		cableSawToVcf = jint(r.j.get(), "cable_id", -1);
		t.check("connect result fields", cableSawToVcf >= 0 && jint(jget(r.j.get(), "from"), "module") == vcoId && jint(jget(r.j.get(), "from"), "output") == sawOut && jstr(jget(r.j.get(), "from"), "name") == "Sawtooth" && jint(jget(r.j.get(), "to"), "module") == vcfId && jstr(jget(r.j.get(), "to"), "name") == "Audio" && jlen(r.get("replaced_cable_ids")) == 0, r.r.content);
		t.check("connect summary", has(r.r.summary, "Sawtooth") && has(r.r.summary, "Audio") && r.r.mutated, r.r.summary);
		t.check("connect created engine and widget cable", APP->engine->getCable(cableSawToVcf) != NULL && rack->getCable(cableSawToVcf) != NULL && cableCount() == 1);

		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":\"lowpass filter\",\"to_module\":%lld,\"input_id\":\"channel\"}", (long long) vcfId, (long long) vcaId));
		t.check("connect by port name (case-insensitive)", r.ok(), r.error());
		cableVcfToVca = jint(r.j.get(), "cable_id", -1);
		(void) cableVcfToVca;
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":\"Envelope\",\"to_module\":%lld,\"input_id\":\"CV\"}", (long long) adsrId, (long long) vcaId));
		t.check("connect ADSR -> VCA CV by name", r.ok(), r.error());
		cableAdsr = jint(r.j.get(), "cable_id", -1);
		t.check("3 cables", cableCount() == 3 && APP->engine->getNumCables() == 3);

		// Duplicate
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":%lld,\"to_module\":%lld,\"input_id\":%lld}", (long long) vcoId, (long long) sawOut, (long long) vcfId, (long long) vcfAudioIn));
		t.check("connect duplicate -> ok 'already connected'", r.ok() && has(jstr(r.j.get(), "note"), "already connected") && jint(r.j.get(), "cable_id") == cableSawToVcf && !r.r.mutated && cableCount() == 3, r.r.content);

		// Occupied input
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":%lld,\"to_module\":%lld,\"input_id\":%lld}", (long long) vcoId, (long long) sqrOut, (long long) vcfId, (long long) vcfAudioIn));
		t.check("connect to occupied input -> error naming the cable", !r.ok() && has(r.error(), std::to_string((long long) cableSawToVcf)) && has(r.error(), "replace") && cableCount() == 3, r.error());

		// Replace
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":%lld,\"to_module\":%lld,\"input_id\":%lld,\"replace\":true}", (long long) vcoId, (long long) sqrOut, (long long) vcfId, (long long) vcfAudioIn));
		t.check("connect replace=true works", r.ok() && jlen(r.get("replaced_cable_ids")) == 1 && json_integer_value(json_array_get(r.get("replaced_cable_ids"), 0)) == cableSawToVcf && cableCount() == 3, r.r.content);
		t.check("replaced cable is gone", rack->getCable(cableSawToVcf) == NULL && APP->engine->getCable(cableSawToVcf) == NULL);
		cableSawToVcf = jint(r.j.get(), "cable_id", -1);

		// Invalid ports / modules: no crash, no change
		size_t cablesBefore = cableCount();
		const std::string base = string::f("\"from_module\":%lld,\"to_module\":%lld", (long long) vcoId, (long long) vcfId);
		r = S.call("connect", "{" + base + ",\"output_id\":99,\"input_id\":0}");
		t.check("connect invalid output index -> error listing ports", !r.ok() && has(r.error(), "out of range") && has(r.error(), "Sawtooth"), r.error());
		r = S.call("connect", "{" + base + ",\"output_id\":-1,\"input_id\":0}");
		t.check("connect negative output index -> error", !r.ok());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":99}");
		t.check("connect invalid input index -> error", !r.ok());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":4294967296}");
		t.check("connect huge input index -> error", !r.ok());
		r = S.call("connect", "{" + base + ",\"output_id\":\"nope\",\"input_id\":0}");
		t.check("connect unknown port name -> error", !r.ok() && has(r.error(), "Available"), r.error());
		r = S.call("connect", "{" + base + ",\"output_id\":0}");
		t.check("connect missing input_id -> error", !r.ok());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":[1]}");
		t.check("connect wrong type input_id -> error", !r.ok());
		r = S.call("connect", string::f("{\"from_module\":123456789,\"output_id\":0,\"to_module\":%lld,\"input_id\":0}", (long long) vcfId));
		t.check("connect invalid from_module -> error", !r.ok() && has(r.error(), "not found"), r.error());
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":0,\"to_module\":-5,\"input_id\":0}", (long long) vcoId));
		t.check("connect invalid to_module -> error", !r.ok());
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":0,\"input_id\":0}", (long long) vcoId));
		t.check("connect missing to_module -> error", !r.ok());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":0,\"color\":\"red\"}");
		t.check("connect invalid color -> error", !r.ok() && has(r.error(), "color"), r.error());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":0,\"color\":\"#ff880000\"}");
		t.check("connect rejects 8-digit (alpha) color", !r.ok() && has(r.error(), "color") && cableCount() == 3, r.error());
		r = S.call("connect", "{" + base + ",\"output_id\":0,\"input_id\":0,\"replace\":\"maybe\"}");
		t.check("connect invalid replace -> error", !r.ok());
		t.check("failed connects changed nothing", cableCount() == cablesBefore && APP->engine->getNumCables() == cablesBefore);

		// Custom color on a CV input (output id by name: Sine)
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":%lld,\"to_module\":%lld,\"input_id\":%lld,\"color\":\"#FF8800\"}", (long long) vcoId, (long long) sineOut, (long long) vcfId, (long long) vcfFreqIn));
		int64_t colorCable = jint(r.j.get(), "cable_id", -1);
		t.check("connect with color", r.ok() && colorCable >= 0, r.error());
		Resp gp = S.call("get_patch", "{}");
		bool colorOk = false;
		size_t i;
		json_t* e;
		json_array_foreach(gp.get("cables"), i, e) {
			if (jint(e, "id") == colorCable && lowercase(jstr(e, "color")) == "#ff8800")
				colorOk = true;
		}
		t.check("cable color is kept", colorOk);

		// ---- set_param -------------------------------------------------------------------
		json_t* cutoffP = entryByName(vcfInfo.get("params"), "Cutoff frequency");
		int64_t cutoffId = jint(cutoffP, "id", -1);
		double cMin = jnum(cutoffP, "min"), cMax = jnum(cutoffP, "max");
		t.check("VCF cutoff param found", cutoffId >= 0 && cMax > cMin);
		double mid = (cMin + cMax) * 0.5;
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%g}", (long long) vcfId, (long long) cutoffId, mid));
		t.check("set_param raw value", r.ok() && std::fabs(jnum(r.j.get(), "value") - mid) < 1e-4 && !jbool(r.j.get(), "clamped", true) && jstr(r.j.get(), "name") == "Cutoff frequency" && !jstr(r.j.get(), "display").empty() && r.r.mutated, r.r.content);
		t.check("set_param result has range and old value", jget(r.j.get(), "old_value") && std::fabs(jnum(r.j.get(), "min") - cMin) < 1e-4 && std::fabs(jnum(r.j.get(), "max") - cMax) < 1e-4);
		t.check("set_param changed the engine param", std::fabs(APP->engine->getModule(vcfId)->params[cutoffId].getValue() - mid) < 1e-4);
		t.check("set_param summary", has(r.r.summary, "Cutoff frequency") && has(r.r.summary, "\xE2\x86\x92"), r.r.summary);

		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%g}", (long long) vcfId, (long long) cutoffId, cMax + 1000.0));
		t.check("set_param clamps above max", r.ok() && jbool(r.j.get(), "clamped") && std::fabs(jnum(r.j.get(), "value") - cMax) < 1e-6 && jnum(r.j.get(), "value") == jnum(r.j.get(), "max"), r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%g}", (long long) vcfId, (long long) cutoffId, cMin - 1000.0));
		t.check("set_param clamps below min", r.ok() && jbool(r.j.get(), "clamped") && jnum(r.j.get(), "value") == jnum(r.j.get(), "min"), r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%g}", (long long) vcfId, (long long) cutoffId, cMin));
		t.check("set_param same value -> not mutated", r.ok() && !r.r.mutated, r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%g}", (long long) vcfId, (long long) cutoffId, mid));
		t.check("set_param back to mid", r.ok() && r.r.mutated);

		// Display value (VCO frequency, in Hz)
		json_t* freqP = entryByName(vcoInfo.get("params"), "Frequency");
		int64_t freqId = jint(freqP, "id", -1);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"display_value\":440}", (long long) vcoId, (long long) freqId));
		double shown = atof(jstr(r.j.get(), "display").c_str());
		t.check("set_param display_value 440 Hz", r.ok() && std::fabs(shown - 440.0) < 4.4 && !jbool(r.j.get(), "clamped", true) && has(jstr(r.j.get(), "display"), "Hz"), r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"display_value\":1e9}", (long long) vcoId, (long long) freqId));
		t.check("set_param display_value far above range -> clamped", r.ok() && jbool(r.j.get(), "clamped") && jnum(r.j.get(), "value") == jnum(r.j.get(), "max"), r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"display_value\":-5}", (long long) vcoId, (long long) freqId));
		t.check("set_param display_value impossible (log of negative) does not crash", r.ok() || !r.error().empty(), r.r.content);

		// By name
		int64_t resId = idByName(vcfInfo.get("params"), "Resonance");
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":\"resonance\",\"value\":0.5}", (long long) vcfId));
		t.check("set_param by name (case-insensitive)", r.ok() && jint(r.j.get(), "param_id") == resId && std::fabs(jnum(r.j.get(), "value") - 0.5) < 1e-6, r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":\"Resonance\",\"value\":\"0.25\"}", (long long) vcfId));
		t.check("set_param numeric string value", r.ok() && std::fabs(jnum(r.j.get(), "value") - 0.25) < 1e-6, r.r.content);

		// Switch with snap
		int64_t fmId = idByName(vcoInfo.get("params"), "FM mode");
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":\"fm mode\",\"value\":0.8}", (long long) vcoId));
		t.check("set_param switch snaps to option", r.ok() && jnum(r.j.get(), "value") == 1.0 && jstr(r.j.get(), "display") == "Linear" && jint(r.j.get(), "param_id") == fmId, r.r.content);

		// Errors
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":999,\"value\":0.5}", (long long) vcfId));
		t.check("set_param invalid param id -> error", !r.ok() && has(r.error(), "out of range"), r.error());
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":-1,\"value\":0.5}", (long long) vcfId));
		t.check("set_param negative param id -> error", !r.ok());
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":\"nonexistent\",\"value\":0.5}", (long long) vcfId));
		t.check("set_param unknown param name -> error listing params", !r.ok() && has(r.error(), "Available") && has(r.error(), "Resonance"), r.error());
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":0,\"value\":0.5,\"display_value\":1}", (long long) vcfId));
		t.check("set_param both value and display_value -> error", !r.ok());
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":0}", (long long) vcfId));
		t.check("set_param neither value nor display_value -> error", !r.ok());
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":0,\"value\":\"abc\"}", (long long) vcfId));
		t.check("set_param non-numeric value -> error", !r.ok());
		r = S.call("set_param", "{\"module_id\":987654321,\"param_id\":0,\"value\":0.5}");
		t.check("set_param invalid module -> error", !r.ok() && has(r.error(), "not found"), r.error());
		r = S.call("set_param", "{\"module_id\":\"abc\",\"param_id\":0,\"value\":0.5}");
		t.check("set_param non-numeric module id -> error", !r.ok());
		r = S.call("set_param", "{\"module_id\":1e300,\"param_id\":0,\"value\":0.5}");
		t.check("set_param absurd module id -> error", !r.ok());

		// numeric-string module id is accepted
		Resp rs = S.call("get_module_info", string::f("{\"module_id\":\"%lld\"}", (long long) vcfId));
		t.check("module id as numeric string accepted", rs.ok(), rs.error());

		// ---- move_module -------------------------------------------------------------------
		GridPos vcoPos = gridPosOf(rack->getModule(vcoId));
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"hp\":%d,\"row\":%d}}", (long long) adsrId, vcoPos.hp, vcoPos.row));
		t.check("move_module into occupied slot -> resolved with note", r.ok() && !jstr(r.j.get(), "note").empty() && findOverlap().empty() && (jint(jget(r.j.get(), "pos"), "hp") != vcoPos.hp || jint(jget(r.j.get(), "pos"), "row") != vcoPos.row), r.r.content + " " + findOverlap());
		GridPos adsrPos = gridPosOf(rack->getModule(adsrId));
		t.check("move_module reported position matches the widget", jint(jget(r.j.get(), "pos"), "hp") == adsrPos.hp && jint(jget(r.j.get(), "pos"), "row") == adsrPos.row);
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"hp\":60,\"row\":0}}", (long long) adsrId));
		t.check("move_module to free slot", r.ok() && jint(jget(r.j.get(), "pos"), "hp") == 60 && jint(jget(r.j.get(), "pos"), "row") == 0 && jstr(r.j.get(), "note").empty() && r.r.mutated && has(r.r.summary, "HP 60"), r.r.content + " " + r.r.summary);
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"hp\":5,\"row\":2}}", (long long) adsrId));
		t.check("move_module to another row", r.ok() && jint(jget(r.j.get(), "pos"), "hp") == 5 && jint(jget(r.j.get(), "pos"), "row") == 2 && gridPosOf(rack->getModule(adsrId)).row == 2, r.r.content);
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"hp\":5,\"row\":2}}", (long long) adsrId));
		t.check("move_module onto itself -> ok, not mutating", r.ok() && !r.r.mutated, r.r.content);
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"hp\":6,\"row\":2}}", (long long) adsrId));
		t.check("move_module overlapping only itself -> exact slot", r.ok() && jint(jget(r.j.get(), "pos"), "hp") == 6 && jstr(r.j.get(), "note").empty(), r.r.content);
		r = S.call("move_module", string::f("{\"module_id\":%lld}", (long long) adsrId));
		t.check("move_module missing position -> error", !r.ok());
		r = S.call("move_module", string::f("{\"module_id\":%lld,\"position\":{\"row\":1}}", (long long) adsrId));
		t.check("move_module position without hp -> error", !r.ok());
		r = S.call("move_module", "{\"module_id\":42,\"position\":{\"hp\":1,\"row\":0}}");
		t.check("move_module invalid module -> error", !r.ok());
		t.check("no overlaps after moves", findOverlap().empty(), findOverlap());

		// ---- get_patch -----------------------------------------------------------------------
		gp = S.call("get_patch", "{}");
		t.check("get_patch counts", gp.ok() && jint(gp.j.get(), "module_count") == 7 && jint(gp.j.get(), "cable_count") == 4 && jlen(gp.get("modules")) == 7 && jlen(gp.get("cables")) == 4, gp.r.content.substr(0, 300));
		t.check("get_patch summary", has(gp.r.summary, "7 modules"), gp.r.summary);
		bool sorted = true;
		int prevRow = -1000, prevHp = -1000;
		bool fieldsOk = true;
		json_t* vcfEntry = NULL;
		json_t* lfoNearEntry = NULL;
		json_array_foreach(gp.get("modules"), i, e) {
			int row = (int) jint(jget(e, "pos"), "row"), hp = (int) jint(jget(e, "pos"), "hp");
			if (row < prevRow || (row == prevRow && hp < prevHp))
				sorted = false;
			prevRow = row;
			prevHp = hp;
			if (jint(e, "id") < 0 || jstr(e, "plugin").empty() || jstr(e, "model").empty() || jstr(e, "name").empty() || jint(e, "width_hp") <= 0 || !jget(e, "bypassed") || !json_is_array(jget(e, "params")))
				fieldsOk = false;
			if (jint(e, "id") == vcfId)
				vcfEntry = e;
			if (jint(e, "id") == lfoNearId)
				lfoNearEntry = e;
		}
		t.check("get_patch modules sorted by row then hp", sorted);
		t.check("get_patch module fields", fieldsOk);
		t.check("get_patch lists only changed params by default", vcfEntry && jlen(jget(vcfEntry, "params")) >= 2 && jlen(jget(vcfEntry, "params")) < jlen(vcfInfo.get("params")) && entryByName(jget(vcfEntry, "params"), "Cutoff frequency") && entryByName(jget(vcfEntry, "params"), "Resonance") && !entryByName(jget(vcfEntry, "params"), "Drive"));
		t.check("get_patch unchanged module has no params listed", lfoNearEntry && jlen(jget(lfoNearEntry, "params")) == 0);
		json_t* cutoffEntry = vcfEntry ? entryByName(jget(vcfEntry, "params"), "Cutoff frequency") : NULL;
		t.check("get_patch param entry has value and display", cutoffEntry && std::fabs(jnum(cutoffEntry, "value") - mid) < 1e-4 && !jstr(cutoffEntry, "display").empty() && jint(cutoffEntry, "id") == cutoffId);
		Resp gpa = S.call("get_patch", "{\"include_all_params\":true}");
		json_t* vcfAll = NULL;
		json_array_foreach(gpa.get("modules"), i, e) {
			if (jint(e, "id") == vcfId)
				vcfAll = e;
		}
		t.check("get_patch include_all_params lists every param", vcfAll && jlen(jget(vcfAll, "params")) == jlen(vcfInfo.get("params")));
		Resp gpf = S.call("get_patch", string::f("{\"module_ids\":[%lld]}", (long long) vcoId));
		bool cablesTouch = true;
		json_array_foreach(gpf.get("cables"), i, e) {
			if (jint(jget(e, "from"), "module") != vcoId && jint(jget(e, "to"), "module") != vcoId)
				cablesTouch = false;
		}
		t.check("get_patch module_ids filter", gpf.ok() && jlen(gpf.get("modules")) == 1 && jint(json_array_get(gpf.get("modules"), 0), "id") == vcoId && jlen(gpf.get("cables")) == 2 && cablesTouch, gpf.r.content.substr(0, 300));
		Resp gpbad = S.call("get_patch", "{\"module_ids\":\"all\"}");
		t.check("get_patch invalid module_ids -> error", !gpbad.ok());
		Resp gpbad2 = S.call("get_patch", "{\"include_all_params\":\"perhaps\"}");
		t.check("get_patch invalid include_all_params -> error", !gpbad2.ok());
		json_t* cable0 = json_array_get(gp.get("cables"), 0);
		t.check("get_patch cable fields", cable0 && jint(cable0, "id") >= 0 && !jstr(cable0, "color").empty() && jget(jget(cable0, "from"), "name") && jget(jget(cable0, "to"), "name") && jget(jget(cable0, "to"), "input") && jget(jget(cable0, "from"), "output"));

		// ---- disconnect ---------------------------------------------------------------------
		r = S.call("disconnect", string::f("{\"cable_id\":%lld}", (long long) colorCable));
		t.check("disconnect by cable_id", r.ok() && jlen(r.get("removed_cable_ids")) == 1 && json_integer_value(json_array_get(r.get("removed_cable_ids"), 0)) == colorCable && cableCount() == 3 && rack->getCable(colorCable) == NULL && APP->engine->getCable(colorCable) == NULL && has(r.r.summary, "Disconnected"), r.r.content);
		r = S.call("disconnect", string::f("{\"cable_id\":%lld}", (long long) colorCable));
		t.check("disconnect unknown cable -> error", !r.ok() && has(r.error(), "get_patch"), r.error());
		r = S.call("disconnect", string::f("{\"to_module\":%lld,\"input_id\":\"CV\"}", (long long) vcaId));
		t.check("disconnect by to_module/input_id", r.ok() && jlen(r.get("removed_cable_ids")) == 1 && json_integer_value(json_array_get(r.get("removed_cable_ids"), 0)) == cableAdsr && cableCount() == 2, r.r.content);
		r = S.call("disconnect", string::f("{\"to_module\":%lld,\"input_id\":\"CV\"}", (long long) vcaId));
		t.check("disconnect empty input -> error", !r.ok());
		r = S.call("disconnect", "{}");
		t.check("disconnect without args -> error", !r.ok());
		r = S.call("disconnect", string::f("{\"to_module\":%lld,\"input_id\":77}", (long long) vcaId));
		t.check("disconnect invalid input -> error", !r.ok());
		r = S.call("disconnect", "{\"cable_id\":\"x\"}");
		t.check("disconnect invalid cable_id type -> error", !r.ok());
		// reconnect so the final patch has cables for the undo/redo checks
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":\"Envelope\",\"to_module\":%lld,\"input_id\":\"CV\"}", (long long) adsrId, (long long) vcaId));
		t.check("reconnect ADSR -> VCA CV", r.ok() && cableCount() == 3, r.error());

		// ---- remove_module -------------------------------------------------------------------
		std::string conf = reg.confirmationFor("remove_module", string::f("{\"module_id\":%lld}", (long long) vcfId));
		t.check("remove_module confirmation non-empty", !conf.empty() && has(conf, "VCF") && has(conf, "2 cables") && has(conf, std::to_string((long long) vcfId)), conf);
		t.check("remove_module confirmation empty for invalid id", reg.confirmationFor("remove_module", "{\"module_id\":5}").empty() && reg.confirmationFor("remove_module", "{bad").empty() && reg.confirmationFor("remove_module", "{}").empty());
		t.check("non-destructive tools have empty confirmation", reg.confirmationFor("connect", "{}").empty() && reg.confirmationFor("get_patch", "{}").empty() && reg.confirmationFor("nonexistent", "{}").empty());

		size_t modsBefore = moduleCount();
		r = S.call("remove_module", string::f("{\"module_id\":%lld}", (long long) vca2Id));
		t.check("remove_module (no cables)", r.ok() && jint(r.j.get(), "removed_module_id") == vca2Id && jint(r.j.get(), "removed_cables") == 0 && moduleCount() == modsBefore - 1 && rack->getModule(vca2Id) == NULL && APP->engine->getModule(vca2Id) == NULL && has(r.r.summary, "removed"), r.r.content);
		r = S.call("remove_module", string::f("{\"module_id\":%lld}", (long long) vca2Id));
		t.check("remove_module twice -> error", !r.ok());

		// Remove a module that has cables: Noise -> VCF Drive
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"Noise"})");
		int64_t noiseId = jint(r.j.get(), "module_id", -1);
		t.check("add Noise", r.ok(), r.error());
		r = S.call("connect", string::f("{\"from_module\":%lld,\"output_id\":0,\"to_module\":%lld,\"input_id\":\"Drive\"}", (long long) noiseId, (long long) vcfId));
		t.check("connect Noise -> VCF Drive", r.ok(), r.error());
		size_t cablesWithNoise = cableCount();
		conf = reg.confirmationFor("remove_module", string::f("{\"module_id\":%lld}", (long long) noiseId));
		t.check("remove_module confirmation counts cables", has(conf, "1 cables") || has(conf, "1 cable"), conf);
		r = S.call("remove_module", string::f("{\"module_id\":%lld}", (long long) noiseId));
		t.check("remove_module with cables removes them", r.ok() && jint(r.j.get(), "removed_cables") == 1 && cableCount() == cablesWithNoise - 1 && APP->engine->getNumCables() == cablesWithNoise - 1, r.r.content);
		t.check("remove_module invalid id -> error", !S.call("remove_module", "{\"module_id\":1}").ok() && !S.call("remove_module", "{}").ok());

		// ---- Result-protocol checks through execute() ------------------------------------------
		ToolResult er = reg.execute("get_patch", "{bad", c1);
		t.check("execute: invalid JSON -> error result", !er.ok && has(er.content, "\"ok\":false") && has(er.content, "Invalid JSON"), er.content);
		er = reg.execute("get_patch", "[1,2]", c1);
		t.check("execute: non-object args -> error result", !er.ok && has(er.content, "expected a JSON object"), er.content);
		er = reg.execute("get_patch", "null", c1);
		t.check("execute: null args -> error result", !er.ok);
		er = reg.execute("get_patch", "", c1);
		t.check("execute: empty args treated as {}", er.ok);
		er = reg.execute("get_patch", "   ", c1);
		t.check("execute: blank args treated as {}", er.ok);
		er = reg.execute("no_such_tool", "{}", c1);
		t.check("execute: unknown tool -> error listing tools", !er.ok && has(er.content, "Unknown tool") && has(er.content, "get_patch"), er.content);
		ToolContext noUndo;
		er = reg.execute("add_module", R"({"plugin":"Fundamental","model":"VCO"})", noUndo);
		t.check("execute: mutating tool without undo context -> error, no crash", !er.ok && moduleCount() == 6, er.content);
	}

	// ---- save_patch ---------------------------------------------------------------------
	{
		t.check("mutations counted before save", c1.mutationsSinceSave > 10);
		if (APP->patch->path.empty()) {
			Resp r = S.call("save_patch", "{}");
			t.check("save_patch on untitled patch without path -> error asking for a path", !r.ok() && has(r.error(), "path"), r.error());
		}
		std::string dirName = "assistant-selftest";
		std::string fileA = system::join(asset::user("patches"), dirName + "/a.vcv");
		std::string fileB = system::join(asset::user("patches"), dirName + "/b.vcv");
		t.check("save_patch confirmation empty for a new file", reg.confirmationFor("save_patch", "{\"path\":\"assistant-selftest/a\"}").empty());
		Resp r = S.call("save_patch", "{\"path\":\"assistant-selftest/a\"}");
		t.check("save_patch relative path", r.ok() && jstr(r.j.get(), "path") == fileA, r.r.content);
		t.check("save_patch file exists", system::isFile(fileA));
		t.check("save_patch resets mutation counter", c1.savedDuringRun && c1.mutationsSinceSave == 0);
		t.check("save_patch sets current patch path", APP->patch->path == fileA);
		t.check("save_patch summary", has(r.r.summary, "Saved a.vcv"), r.r.summary);
		t.check("save_patch is not a patch mutation", !r.r.mutated);
		r = S.call("save_patch", "{\"path\":\"assistant-selftest/b.vcv\"}");
		t.check("save_patch with explicit extension", r.ok() && system::isFile(fileB), r.r.content);
		std::string conf = reg.confirmationFor("save_patch", "{\"path\":\"assistant-selftest/a\"}");
		t.check("save_patch overwrite of a different existing file needs confirmation", has(conf, "Overwrite existing file") && has(conf, fileA), conf);
		t.check("save_patch onto the current file needs no confirmation", reg.confirmationFor("save_patch", "{\"path\":\"assistant-selftest/b\"}").empty() && reg.confirmationFor("save_patch", "{}").empty());
		r = S.call("save_patch", "{}");
		t.check("save_patch without path overwrites the current file", r.ok() && jstr(r.j.get(), "path") == fileB, r.r.content);
		r = S.call("save_patch", "{\"path\":\"/tmp/evil.vcv\"}");
		t.check("save_patch absolute path rejected", !r.ok());
		r = S.call("save_patch", "{\"path\":\"../evil\"}");
		t.check("save_patch '..' rejected", !r.ok());
		r = S.call("save_patch", "{\"path\":5}");
		t.check("save_patch invalid path type rejected", !r.ok());
		// The saved patch must be loadable JSON/archive: at least non-empty
		t.check("saved patch file is non-empty", system::isFile(fileA));
		// The saved file must not carry "unsaved": true (setSaved() has to run before save())
		t.check("patch is marked saved after save_patch", APP->history->isSaved());
		{
			std::string tmpDir = system::join(asset::user("patches"), dirName + "/unpack");
			bool unsavedKey = true, readable = false;
			try {
				system::createDirectories(tmpDir);
				system::unarchiveToDirectory(fileB, tmpDir);
				std::vector<uint8_t> data = system::readFile(system::join(tmpDir, "patch.json"));
				std::string text(data.begin(), data.end());
				readable = !data.empty();
				unsavedKey = text.find("\"unsaved\"") != std::string::npos;
			}
			catch (const std::exception&) {
			}
			system::removeRecursively(tmpDir);
			t.check("saved patch.json has no 'unsaved' flag", readable && !unsavedKey);
		}
	}

	// ---- Undo / redo ---------------------------------------------------------------------
	{
		Snapshot built = takeSnapshot();
		t.check("build action recorded undo actions", !a1->isEmpty());
		t.check("build has 6 modules and 3 cables", built.modules.size() == 6 && built.cables.size() == 3, string::f("%d modules, %d cables", (int) built.modules.size(), (int) built.cables.size()));
		APP->history->push(a1);

		APP->history->undo();
		t.check("undo once -> 0 modules", moduleCount() == 0 && APP->engine->getNumModules() == 0);
		t.check("undo once -> 0 cables", cableCount() == 0 && APP->engine->getNumCables() == 0);
		APP->history->redo();
		std::string d = diffSnapshots(built, takeSnapshot());
		t.check("redo restores modules, cables, params and ids", d.empty(), d);
		t.check("redo: no overlaps", findOverlap().empty(), findOverlap());
		t.check("redo: engine matches widgets", APP->engine->getNumModules() == moduleCount() && APP->engine->getNumCables() == cableCount());

		// Undo both actions: the originally loaded patch comes back
		APP->history->undo();
		APP->history->undo();
		d = diffSnapshots(originalSnapshot, takeSnapshot());
		t.check("undo of clear restores the original patch", d.empty() && moduleCount() == originalModules, d);
		t.check("original cables restored", cableCount() == originalCables && APP->engine->getNumCables() == originalCables);

		// And forward again
		APP->history->redo();
		t.check("redo of clear -> 0 modules", moduleCount() == 0);
		APP->history->redo();
		d = diffSnapshots(built, takeSnapshot());
		t.check("final redo restores the built patch", d.empty(), d);
	}

	// ---- Cleanup -------------------------------------------------------------------------
	try {
		system::removeRecursively(system::join(asset::user("patches"), "assistant-selftest"));
	}
	catch (const std::exception&) {
	}
	APP->patch->path = originalPatchPath;
}


// ---- Driver --------------------------------------------------------------------------

typedef void (*ScenarioFn)(SelfTestRun& t);

struct ScenarioEntry {
	const char* name;
	ScenarioFn fn;
};


/** Scenarios selected by RACK_ASSISTANT_SELFTEST ("tools", "mock" or "all").
Append new scenarios here (the "mock" controller scenarios are added by a later step). */
std::vector<ScenarioEntry> selectScenarios(const std::string& mode) {
	std::vector<ScenarioEntry> list;
	if (mode == "tools" || mode == "all")
		list.push_back({"tools", toolsScenario});
	return list;
}


void finish(SelfTestRun& run) {
	INFO("[assistant selftest] done: %d passed, %d failed", run.passed, run.failed);

	json_t* root = json_object();
	setInt(root, "passed", run.passed);
	setInt(root, "failed", run.failed);
	json_t* failures = json_array();
	for (const std::string& f : run.failures)
		appendJson(failures, jStr(f));
	setJson(root, "failures", failures);
	std::string path = asset::user("assistant-selftest-result.json");
	try {
		system::createDirectories(system::getDirectory(path));
	}
	catch (const std::exception&) {
	}
	if (json_dump_file(root, path.c_str(), JSON_INDENT(2)) != 0)
		WARN("[assistant selftest] could not write %s", path.c_str());
	json_decref(root);

	// Close the window; Rack shuts down normally
	if (APP->window && APP->window->win)
		glfwSetWindowShouldClose(APP->window->win, GLFW_TRUE);
}


} // namespace


void sceneStepHook() {
	// state: 0 = not initialized, 1 = waiting for frames, 2 = done / disabled
	static int state = 0;
	static int frames = 0;
	static std::string mode;

	if (state == 2)
		return;
	if (state == 0) {
		const char* env = getenv("RACK_ASSISTANT_SELFTEST");
		mode = env ? env : "";
		if (mode != "tools" && mode != "mock" && mode != "all") {
			state = 2;
			return;
		}
		INFO("[assistant selftest] enabled (mode %s)", mode.c_str());
		// Remove a stale result so a crash cannot leave an old pass behind
		std::remove(asset::user("assistant-selftest-result.json").c_str());
		state = 1;
	}

	if (++frames < 30)
		return;
	state = 2;

	SelfTestRun run;
	for (const ScenarioEntry& s : selectScenarios(mode)) {
		INFO("[assistant selftest] scenario %s", s.name);
		try {
			s.fn(run);
		}
		catch (const std::exception& e) {
			run.check(std::string("scenario ") + s.name + " completed", false, std::string("exception: ") + e.what());
		}
		catch (...) {
			run.check(std::string("scenario ") + s.name + " completed", false, "unknown exception");
		}
	}
	finish(run);
}


} // namespace assistant
} // namespace rack

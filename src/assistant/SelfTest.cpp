#include <assistant/SelfTest.hpp>
#include <assistant/Controller.hpp>
#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>
#include <assistant/SystemPrompt.hpp>
#include <assistant/SettingsDialog.hpp>
#include <assistant/Panel.hpp>
#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#if !defined ARCH_WIN
#include <unistd.h>
#endif

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

		// Multi-word query that the fuzzy index alone does not match falls back to the single words
		Resp rmw = S.call("search_modules", R"({"query":"xyzzy filter"})");
		t.check("search_modules multi-word fallback finds filters", rmw.ok() && jlen(rmw.get("results")) >= 1, rmw.r.content);

		// Words that match the same model rank first: a filter must be within the default 12 results
		{
			Resp rlp = S.call("search_modules", R"({"query":"low pass filter"})");
			bool filterInTop = false;
			json_t* results = rlp.get("results");
			for (size_t i = 0; results && i < jlen(results); i++) {
				json_t* item = json_array_get(results, i);
				std::string nm = jstr(item, "name");
				if (nm.find("Filter") != std::string::npos || nm.find("VCF") != std::string::npos || nm.find("filter") != std::string::npos)
					filterInTop = true;
			}
			t.check("search_modules 'low pass filter' lists a filter among the first results", rlp.ok() && filterInTop, rlp.r.content);
		}

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
		Resp rph = S.call("get_module_info", R"({"module_id":0,"plugin":"Fundamental","model":"VCF"})");
		t.check("get_module_info placeholder module_id + plugin/model falls back to the catalog", rph.ok() && jstr(rph.j.get(), "model") == "VCF", rph.error());
		Resp rbadId = S.call("get_module_info", R"({"module_id":0})");
		t.check("get_module_info unknown module_id alone -> error", !rbadId.ok());

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
		t.check("failed add_module calls did not add modules", moduleCount() == before && APP->engine->getNumModules() == before);
		// An unknown near_module_id (models send placeholders such as 0) is only a placement hint: ignored with a note
		r = S.call("add_module", R"({"plugin":"Fundamental","model":"VCO","near_module_id":99999999})");
		t.check("add_module unknown near_module_id -> ok with note", r.ok() && !jstr(r.j.get(), "note").empty() && moduleCount() == before + 1, r.r.content);
		if (r.ok()) {
			Resp rm = S.call("remove_module", string::f("{\"module_id\":%lld}", (long long) jint(r.j.get(), "module_id")));
			t.check("placeholder module removed again", rm.ok() && moduleCount() == before, rm.r.content);
		}
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
		t.check("set_param both value and display_value -> display_value wins, with a note", r.ok() && !jstr(r.j.get(), "note").empty() && std::fabs(jnum(r.j.get(), "value") - 0.5) > 1e-6, r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":0.5,\"display_value\":0}", (long long) vcfId, (long long) cutoffId));
		t.check("set_param value + placeholder display_value 0 -> value used", r.ok() && std::fabs(jnum(r.j.get(), "value") - 0.5) < 1e-6 && !jstr(r.j.get(), "note").empty(), r.r.content);
		r = S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":0,\"display_value\":1000}", (long long) vcfId, (long long) cutoffId));
		t.check("set_param placeholder value 0 + display_value -> display_value used", r.ok() && std::fabs(jnum(r.j.get(), "value")) > 1e-6 && has(jstr(r.j.get(), "display"), "Hz"), r.r.content);
		// restore the cutoff for the checks below
		S.call("set_param", string::f("{\"module_id\":%lld,\"param_id\":%lld,\"value\":%.9g}", (long long) vcfId, (long long) cutoffId, (double) mid));
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
		t.check("remove_module confirmation non-empty", !conf.empty() && has(conf, "VCF") && has(conf, "2 cables") && has(conf, "HP"), conf);
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
#if !defined ARCH_WIN
		{
			// A symlink inside the patches folder must not lead the file outside of it
			std::string outside = system::join(asset::user(), dirName + "-outside");
			std::string link = system::join(asset::user("patches"), dirName + "/link");
			system::createDirectories(outside);
			bool linked = symlink(outside.c_str(), link.c_str()) == 0;
			t.check("selftest created a symlink", linked);
			if (linked) {
				r = S.call("save_patch", "{\"path\":\"assistant-selftest/link/x\"}");
				t.check("save_patch through a symlink leaving the patches folder is rejected", !r.ok() && !system::exists(system::join(outside, "x.vcv")), r.r.content);
				unlink(link.c_str());
			}
			system::removeRecursively(outside);
		}
#endif
		r = S.call("save_patch", "{\"path\":\"assistant-selftest/c.json\"}");
		t.check("save_patch forces the .vcv extension", r.ok() && has(jstr(r.j.get(), "path"), "c.json.vcv") && system::isFile(system::join(asset::user("patches"), dirName + "/c.json.vcv")), r.r.content);
		// Keep the saved patch path on b.vcv for the following checks
		r = S.call("save_patch", "{\"path\":\"assistant-selftest/b\"}");
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


// ---- Mock controller scenarios (step-driven across frames) ---------------------------

const double MOCK_SCENARIO_TIMEOUT_SEC = 20.0;

const char* const RETRY_ERROR_BODY = R"({"error":{"message":"Function tools with reasoning_effort are not supported for gpt-5.6-terra in /v1/chat/completions. To use function tools, use /v1/responses or set reasoning_effort to 'none'."}})";


/** Chat completion body with plain text content (no quotes or backslashes in `content`). */
std::string chatBody(const std::string& content, const std::string& finish) {
	return std::string("{\"model\":\"selftest\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"") + content + "\"},\"finish_reason\":\"" + finish + "\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15,\"cost\":0.0123}}";
}


/** What the scripted test client saw. Written by worker threads, read by the UI thread. */
struct ClientProbe {
	std::atomic<int> calls;
	std::atomic<int> noneCalls;
	std::atomic<int> lastRequestSize;
	std::mutex mutex;
	std::string lastModel;
	ChatRequest first;

	ClientProbe() : calls(0), noneCalls(0), lastRequestSize(0) {}
};

typedef std::function<ChatResponse(const ClientOptions&, const ChatRequest&, int)> ScriptFn;

struct ScriptClient : LlmClient {
	ClientOptions options;
	ScriptFn fn;
	std::shared_ptr<ClientProbe> probe;

	ChatResponse complete(const ChatRequest& req, const std::atomic<bool>& cancel) override {
		int index = probe->calls.fetch_add(1);
		probe->lastRequestSize = (int) req.messages.size();
		{
			std::lock_guard<std::mutex> lock(probe->mutex);
			if (index == 0)
				probe->first = req;
			probe->lastModel = options.config.model;
		}
		if (options.config.reasoningEffort == "none")
			probe->noneCalls++;
		return fn(options, req, index);
	}
};


const ChatEntry* findEntryWith(Controller* c, ChatEntry::Kind kind, const std::string& sub) {
	for (const ChatEntry& e : c->getEntries()) {
		if (e.kind == kind && has(e.text, sub))
			return &e;
	}
	return NULL;
}

int countKind(Controller* c, ChatEntry::Kind kind) {
	int n = 0;
	for (const ChatEntry& e : c->getEntries()) {
		if (e.kind == kind)
			n++;
	}
	return n;
}

int countRole(Controller* c, ChatMessage::Role role) {
	int n = 0;
	for (const ChatMessage& m : c->getMessages()) {
		if (m.role == role)
			n++;
	}
	return n;
}

const ChatEntry* lastOfKind(Controller* c, ChatEntry::Kind kind) {
	const std::vector<ChatEntry>& es = c->getEntries();
	for (size_t i = es.size(); i > 0; i--) {
		if (es[i - 1].kind == kind)
			return &es[i - 1];
	}
	return NULL;
}

std::string dumpEntries(Controller* c) {
	static const char* kinds[] = {"USER", "ASSISTANT", "ACTIONS", "ERROR", "INFO", "CONFIRM"};
	std::string s;
	for (const ChatEntry& e : c->getEntries()) {
		s += std::string("[") + kinds[(int) e.kind] + ": " + e.text;
		for (const ChatEntry::Action& a : e.actions)
			s += " {" + a.text + "}";
		s += "] ";
	}
	return truncateUtf8(s, 600);
}

/** Every tool call of every assistant message must be answered by a tool message right after it. */
bool historyValid(const std::vector<ChatMessage>& ms) {
	for (size_t i = 0; i < ms.size(); i++) {
		if (ms[i].role != ChatMessage::ASSISTANT)
			continue;
		for (const ToolCall& tc : ms[i].toolCalls) {
			bool found = false;
			for (size_t j = i + 1; j < ms.size() && ms[j].role == ChatMessage::TOOL; j++) {
				if (ms[j].toolCallId == tc.id)
					found = true;
			}
			if (!found)
				return false;
		}
	}
	return true;
}

bool toolMessageHas(Controller* c, const std::string& sub) {
	for (const ChatMessage& m : c->getMessages()) {
		if (m.role == ChatMessage::TOOL && has(m.content, sub))
			return true;
	}
	return false;
}

int countModels(const std::string& plugin, const std::string& model) {
	int n = 0;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (mw->model->plugin->slug == plugin && mw->model->slug == model)
			n++;
	}
	return n;
}

/** Empties the patch with one undoable action, like a user would. */
void prepareEmptyPatch() {
	APP->scene->rack->deselectAll();
	if (moduleCount() == 0)
		return;
	history::ComplexAction* a = new history::ComplexAction;
	a->name = "selftest prepare";
	ToolContext ctx;
	ctx.undo = a;
	defaultRegistry().execute("clear_patch", "{}", ctx);
	if (!a->isEmpty())
		APP->history->push(a);
	else
		delete a;
}

/** Adds a module through the tool registry (one undo action). Returns its id or -1. */
int64_t addModuleDirect(const std::string& plugin, const std::string& model) {
	history::ComplexAction* a = new history::ComplexAction;
	a->name = "selftest add";
	ToolContext ctx;
	ctx.undo = a;
	std::string args = "{\"plugin\":\"" + plugin + "\",\"model\":\"" + model + "\"}";
	ToolResult r = defaultRegistry().execute("add_module", args, ctx);
	int64_t id = -1;
	json_error_t err;
	JsonPtr j(json_loads(r.content.c_str(), 0, &err));
	if (r.ok && j)
		id = jint(j.get(), "module_id", -1);
	if (!a->isEmpty())
		APP->history->push(a);
	else
		delete a;
	return id;
}


struct FileBackup {
	std::string path;
	bool existed = false;
	std::string data;

	void backup(const std::string& p) {
		path = p;
		std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
		existed = (bool) f;
		if (f) {
			std::stringstream ss;
			ss << f.rdbuf();
			data = ss.str();
		}
	}
	void restore() {
		if (path.empty())
			return;
		if (existed) {
			std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
			f << data;
		}
		else {
			std::remove(path.c_str());
		}
	}
};


struct MockScenario {
	SelfTestRun* t = NULL;
	Controller* c = NULL;
	int phase = 0;
	int phaseFrames = 0;
	std::shared_ptr<ClientProbe> probe;

	virtual ~MockScenario() {}
	virtual const char* name() const = 0;
	/** Adjust the config used by the dedicated controller (mock = true is preset). */
	virtual void configure(Config& cfg) {}
	/** Called once, after the controller exists. */
	virtual void init() {}
	/** Called every frame after Controller::step(). Returns true when the scenario is finished. */
	virtual bool step() = 0;

	void ck(const std::string& what, bool ok, const std::string& detail = "") {
		t->check(std::string("mock/") + name() + ": " + what, ok, detail);
	}
	void next() {
		phase++;
		phaseFrames = 0;
	}
	/** Replaces the mock client by a scripted one. */
	void useScript(ScriptFn fn) {
		probe = std::make_shared<ClientProbe>();
		std::shared_ptr<ClientProbe> p = probe;
		c->setClientFactory([p, fn](const ClientOptions& o) -> std::shared_ptr<LlmClient> {
			ScriptClient* s = new ScriptClient;
			s->options = o;
			s->fn = fn;
			s->probe = p;
			return std::shared_ptr<LlmClient>(s);
		});
	}
	/** Waits for a finished run. Returns true once idle. */
	bool idle() const {
		return !c->isBusy();
	}
};


struct PromptScenario : MockScenario {
	const char* name() const override {
		return "prompt";
	}
	bool step() override {
		std::string def = defaultSystemPrompt();
		ck("default prompt covers the tool workflow", has(def, "get_patch") && has(def, "search_modules") && has(def, "get_module_info") && has(def, "add_module") && has(def, "connect"));
		ck("default prompt covers modular basics", has(def, "1 V/oct") && has(def, "gate") && has(def, "VCO") && has(def, "VCF") && has(def, "VCA") && has(def, "ADSR") && has(def, "LFO"));
		ck("default prompt covers the genre recipes", has(def, "Tekno") && has(def, "303") && has(def, "kick") && has(def, "hoover") && has(def, "Dub delay") && has(def, "190"));
		ck("default prompt mentions the audio interface", has(def, "audio interface"));
		ck("default prompt has a sane size", def.size() > 3000 && def.size() < 20000, std::to_string(def.size()));

		std::string env = environmentBlock();
		ck("environment block has the version", has(env, APP_VERSION));
		ck("environment block lists Fundamental", has(env, "Fundamental"), truncateUtf8(env, 300));
		ck("environment block lists Core", has(env, "- Core"));

		// User-editable file: created when missing, used when present, default when empty
		std::string path = systemPromptPath();
		std::remove(path.c_str());
		std::string loaded = loadSystemPrompt();
		bool created = false;
		{
			std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			created = (bool) f && has(ss.str(), "built-in assistant");
		}
		ck("loadSystemPrompt creates the file with the default", created && loaded == def);
		{
			std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
			f << "Custom prompt for the selftest.";
		}
		ck("loadSystemPrompt reads the edited file", loadSystemPrompt() == "Custom prompt for the selftest.");
		{
			std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
			f << "  \n";
		}
		ck("empty prompt file falls back to the default", loadSystemPrompt() == def);
		std::remove(path.c_str());
		return true;
	}
};


struct BuildScenario : MockScenario {
	const char* name() const override {
		return "build";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("patch is empty before the run", moduleCount() == 0);
				ck("send accepted", c->send("/mock-build", false));
				ck("waiting for HTTP after send", c->getState() == Controller::WAITING_HTTP && c->isBusy());
				ck("second send is rejected while busy", !c->send("another", false));
				ck("status shows the wait", has(c->getStatusText(), "Thinking"), c->getStatusText());
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("run ends IDLE", c->getState() == Controller::IDLE);
				ck("no error entries", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("VCO and VCF were added", countModels("Fundamental", "VCO") >= 1 && countModels("Fundamental", "VCF") >= 1, dumpEntries(c));
				ck("at least one cable", cableCount() >= 1);
				int actions = 0;
				int withText = 0;
				for (const ChatEntry& e : c->getEntries()) {
					if (e.kind != ChatEntry::ACTIONS)
						continue;
					for (const ChatEntry::Action& a : e.actions) {
						actions++;
						if (!a.text.empty() && a.ok)
							withText++;
					}
				}
				ck("ACTIONS entries list summaries", actions >= 5 && withText == actions, string::f("%d actions, %d ok", actions, withText));
				const ChatEntry* fin = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("final ASSISTANT text", fin && has(fin->text, "Mock: built VCO"), dumpEntries(c));
				ck("API history is valid", historyValid(c->getMessages()));
				ck("status shows the last run", has(c->getStatusText(), "Last run: 5 requests"), c->getStatusText());
				ck("revision advanced", c->getRevision() > 0);
				ck("run recorded one undo action", APP->history->canUndo() && APP->history->getUndoName() == "assistant changes", APP->history->getUndoName());

				Snapshot built = takeSnapshot();
				APP->history->undo();
				ck("undo once -> 0 modules", moduleCount() == 0 && APP->engine->getNumModules() == 0);
				ck("undo once -> 0 cables", cableCount() == 0 && APP->engine->getNumCables() == 0);
				APP->history->redo();
				std::string d = diffSnapshots(built, takeSnapshot());
				ck("redo restores the built patch", d.empty() && moduleCount() >= 2, d);
				return true;
			}
		}
		return false;
	}
};


struct DeleteScenario : MockScenario {
	int64_t moduleId = -1;

	const char* name() const override {
		return "delete";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				moduleId = addModuleDirect("Fundamental", "VCO");
				ck("module added for the test", moduleId >= 0 && countModels("Fundamental", "VCO") == 1);
				ck("send accepted", c->send("/mock-delete", false));
				next();
			} break;
			case 1: {
				if (c->getState() != Controller::WAITING_CONFIRM) {
					if (idle()) {
						ck("reached WAITING_CONFIRM", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("CONFIRM entry is pending", ce && ce->confirmState == ChatEntry::PENDING && has(ce->text, "Remove module"), dumpEntries(c));
				ck("status asks for confirmation", has(c->getStatusText(), "confirmation"), c->getStatusText());
				ck("module not removed before the answer", countModels("Fundamental", "VCO") == 1);
				if (ce) {
					c->confirm(ce->id + 1000, true);
					ck("unknown entry id is ignored", c->getState() == Controller::WAITING_CONFIRM);
					c->confirm(ce->id, false);
				}
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("deny keeps the module", countModels("Fundamental", "VCO") == 1);
				ck("CONFIRM entry shows DENIED", ce && ce->confirmState == ChatEntry::DENIED);
				ck("tool result says declined", toolMessageHas(c, "declined"));
				const ChatEntry* fin = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("run ends with a text", fin && !fin->text.empty() && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("API history is valid", historyValid(c->getMessages()));
				ck("answering again is ignored", (c->confirm(ce ? ce->id : 0, true), countModels("Fundamental", "VCO") == 1));
				ck("second send accepted", c->send("/mock-delete", false));
				next();
			} break;
			case 3: {
				if (c->getState() != Controller::WAITING_CONFIRM) {
					if (idle()) {
						ck("reached WAITING_CONFIRM again", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("second CONFIRM entry is pending", ce && ce->confirmState == ChatEntry::PENDING);
				if (ce)
					c->confirm(ce->id, true);
				next();
			} break;
			case 4: {
				if (!idle())
					return false;
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("allow removes the module", countModels("Fundamental", "VCO") == 0 && moduleCount() == 0);
				ck("CONFIRM entry shows ALLOWED", ce && ce->confirmState == ChatEntry::ALLOWED);
				ck("removal listed in ACTIONS", findEntryWith(c, ChatEntry::ACTIONS, "") != NULL);
				ck("API history is valid", historyValid(c->getMessages()));
				APP->history->undo();
				ck("undo restores the removed module", countModels("Fundamental", "VCO") == 1);
				return true;
			}
		}
		return false;
	}
};


struct DeleteNoConfirmScenario : MockScenario {
	bool sawConfirm = false;

	const char* name() const override {
		return "delete-no-confirm";
	}
	void configure(Config& cfg) override {
		cfg.confirmDestructive = false;
	}
	bool step() override {
		if (c->getState() == Controller::WAITING_CONFIRM)
			sawConfirm = true;
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("module added for the test", addModuleDirect("Fundamental", "VCO") >= 0);
				ck("send accepted", c->send("/mock-delete", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("no confirmation requested", !sawConfirm && countKind(c, ChatEntry::CONFIRM) == 0);
				ck("module removed", moduleCount() == 0);
				return true;
			}
		}
		return false;
	}
};


struct BadArgsScenario : MockScenario {
	int actionIndexBefore = 0;

	const char* name() const override {
		return "badargs";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				actionIndexBefore = APP->history->actionIndex;
				ck("send accepted", c->send("/mock-badargs", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				bool failure = false;
				bool short80 = true;
				for (const ChatEntry& e : c->getEntries()) {
					if (e.kind != ChatEntry::ACTIONS)
						continue;
					for (const ChatEntry::Action& a : e.actions) {
						if (!a.ok && a.text.find("\xe2\x9c\x97 add_module: ") == 0) {
							failure = true;
							// "✗ add_module: " is 16 bytes; the error text is cut at ~80 bytes (+ ellipsis)
							if (a.text.size() > 16 + 80 + 3)
								short80 = false;
						}
					}
				}
				ck("ACTIONS contains a failure", failure, dumpEntries(c));
				ck("failure text is short", short80);
				const ChatEntry* fin = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("run ends with the model's text", fin && has(fin->text, "invalid arguments") && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("tool result is an error object", toolMessageHas(c, "\"ok\":false"));
				ck("patch unchanged", moduleCount() == 0);
				ck("API history is valid", historyValid(c->getMessages()));
				ck("a run without changes pushes no undo action", APP->history->actionIndex == actionIndexBefore);
				return true;
			}
		}
		return false;
	}
};


struct ErrorScenario : MockScenario {
	const char* name() const override {
		return "error-402";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("/mock-error 402", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				const ChatEntry* e = lastOfKind(c, ChatEntry::ERROR);
				ck("ERROR entry mentions credits", e && has(e->text, "credits"), dumpEntries(c));
				ck("no assistant entry", countKind(c, ChatEntry::ASSISTANT) == 0);
				ck("only the user message in the conversation", c->getMessages().size() == 1);
				ck("status is empty after a failed run", c->getStatusText().empty(), c->getStatusText());
				ck("a new send works after an error", c->send("hello", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				const ChatEntry* e = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("reply after the error", e && has(e->text, "Mock reply: hello"), dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


struct CancelScenario : MockScenario {
	const char* name() const override {
		return "cancel";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("send accepted", c->send("/mock-build", false));
				ck("waiting for HTTP", c->getState() == Controller::WAITING_HTTP);
				next();
			} break;
			case 1: {
				if (phaseFrames < 2)
					return false;
				if (!c->isBusy()) {
					ck("run still active after 2 frames", false, dumpEntries(c));
					return true;
				}
				auto t0 = std::chrono::steady_clock::now();
				c->cancel();
				double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				ck("cancel returns immediately", ms < 200.0, string::f("%.0f ms", ms));
				ck("state is IDLE right after cancel", c->getState() == Controller::IDLE);
				const ChatEntry* e = lastOfKind(c, ChatEntry::INFO);
				ck("INFO Cancelled.", e && e->text == "Cancelled.", dumpEntries(c));
				ck("no patch changes", moduleCount() == 0);
				ck("cancel when idle is a no-op", (c->cancel(), c->getState() == Controller::IDLE && countKind(c, ChatEntry::INFO) == 1));
				ck("a new send works right after cancel", c->send("hello", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				const ChatEntry* e = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("reply after cancel", e && has(e->text, "Mock reply: hello"), dumpEntries(c));
				ck("no stale response of the cancelled request", countKind(c, ChatEntry::ASSISTANT) == 1 && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


struct CancelConfirmScenario : MockScenario {
	const char* name() const override {
		return "cancel-confirm";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("module added for the test", addModuleDirect("Fundamental", "VCO") >= 0);
				ck("send accepted", c->send("/mock-delete", false));
				next();
			} break;
			case 1: {
				if (c->getState() != Controller::WAITING_CONFIRM) {
					if (idle()) {
						ck("reached WAITING_CONFIRM", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				c->cancel();
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("state is IDLE", c->getState() == Controller::IDLE);
				ck("CONFIRM entry is CANCELLED", ce && ce->confirmState == ChatEntry::CANCELLED);
				ck("INFO Cancelled.", findEntryWith(c, ChatEntry::INFO, "Cancelled.") != NULL);
				ck("module still there", countModels("Fundamental", "VCO") == 1);
				ck("remaining tool call got a cancelled result", toolMessageHas(c, "Cancelled by the user"));
				ck("API history is valid", historyValid(c->getMessages()));
				return true;
			}
		}
		return false;
	}
};


struct LoopScenario : MockScenario {
	const char* name() const override {
		return "loop";
	}
	void configure(Config& cfg) override {
		cfg.maxToolRounds = 3;
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("/mock-loop", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("max-rounds INFO", findEntryWith(c, ChatEntry::INFO, "Stopped after 3 tool rounds") != NULL, dumpEntries(c));
				ck("exactly 3 model rounds", countRole(c, ChatMessage::ASSISTANT) == 3, std::to_string(countRole(c, ChatMessage::ASSISTANT)));
				ck("exactly 3 tool results", countRole(c, ChatMessage::TOOL) == 3);
				ck("3 ACTIONS entries", countKind(c, ChatEntry::ACTIONS) == 3);
				ck("API history is valid", historyValid(c->getMessages()));
				ck("run ends IDLE", c->getState() == Controller::IDLE);
				return true;
			}
		}
		return false;
	}
};


struct RetryScenario : MockScenario {
	const char* name() const override {
		return "retry-none";
	}
	void init() override {
		useScript([](const ClientOptions& o, const ChatRequest&, int) -> ChatResponse {
			if (o.config.reasoningEffort == "none")
				return parseResponse(200, chatBody("Retry worked.", "stop"));
			return parseResponse(400, RETRY_ERROR_BODY);
		});
	}
	bool step() override {
		const std::string retryText = "The provider does not support reasoning together with tools for this model; retried with reasoning effort 'none'. Change it in Settings to skip the extra request.";
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("hello", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("INFO about the retry", findEntryWith(c, ChatEntry::INFO, retryText) != NULL, dumpEntries(c));
				const ChatEntry* e = lastOfKind(c, ChatEntry::ASSISTANT);
				ck("final ASSISTANT text", e && e->text == "Retry worked.", dumpEntries(c));
				ck("no ERROR entry", countKind(c, ChatEntry::ERROR) == 0);
				ck("exactly two requests, one with 'none'", probe->calls == 2 && probe->noneCalls == 1, string::f("%d calls, %d none", probe->calls.load(), probe->noneCalls.load()));
				ck("usage of the successful request is summarized", c->getStatusText() == "Last run: 1 request \xc2\xb7 15 tokens \xc2\xb7 $0.0123 \xc2\xb7 effort none (provider fallback)", c->getStatusText());
				ck("config is unchanged", c->getConfig().reasoningEffort == "medium");
				ck("second send accepted", c->send("again", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				int retries = 0;
				for (const ChatEntry& e : c->getEntries()) {
					if (e.kind == ChatEntry::INFO && e.text == retryText)
						retries++;
				}
				ck("override sticks for the next run (no second failing request, no second INFO)", retries == 1 && probe->calls == 3, string::f("%d retries, %d calls", retries, probe->calls.load()));
				ck("the active effort fallback stays visible in the status", has(c->getStatusText(), "effort none (provider fallback)"), c->getStatusText());
				Config cfg = c->getConfig();
				cfg.reasoningEffort = "off";
				c->setConfig(cfg);
				ck("third send accepted", c->send("third", false));
				next();
			} break;
			case 3: {
				if (!idle())
					return false;
				const ChatEntry* e = lastOfKind(c, ChatEntry::ERROR);
				ck("effort 'off' is not retried: error shown", e && has(e->text, "rejected the request") && probe->calls == 4, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


struct LengthScenario : MockScenario {
	const char* name() const override {
		return "length";
	}
	void init() override {
		useScript([](const ClientOptions&, const ChatRequest&, int) -> ChatResponse {
			return parseResponse(200, chatBody("Partial text", "length"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("hello", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("partial text shown", findEntryWith(c, ChatEntry::ASSISTANT, "Partial text") != NULL);
				ck("INFO cut off", findEntryWith(c, ChatEntry::INFO, "The response was cut off (max_tokens).") != NULL, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


struct EmptyReplyScenario : MockScenario {
	const char* name() const override {
		return "empty-reply";
	}
	void init() override {
		useScript([](const ClientOptions&, const ChatRequest&, int) -> ChatResponse {
			return parseResponse(200, chatBody("", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("hello", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("INFO empty reply", findEntryWith(c, ChatEntry::INFO, "The model returned an empty reply.") != NULL, dumpEntries(c));
				ck("no assistant entry", countKind(c, ChatEntry::ASSISTANT) == 0 && countKind(c, ChatEntry::ERROR) == 0);
				ck("empty reply not kept in the conversation", c->getMessages().size() == 1);
				return true;
			}
		}
		return false;
	}
};


struct NewChatScenario : MockScenario {
	const char* name() const override {
		return "new-chat";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("hello", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("entries and messages exist", c->getEntries().size() >= 2 && c->getMessages().size() >= 2);
				uint64_t rev = c->getRevision();
				uint64_t lastId = c->getEntries().back().id;
				c->newChat();
				ck("newChat clears entries and messages", c->getEntries().empty() && c->getMessages().empty());
				ck("newChat bumps the revision", c->getRevision() > rev);
				ck("state IDLE", c->getState() == Controller::IDLE);
				ck("status empty", c->getStatusText().empty(), c->getStatusText());
				// Entry ids keep increasing across chats
				ck("send after newChat", c->send("again", false));
				ck("entry ids stay unique", c->getEntries().back().id > lastId);
				c->newChat();
				ck("newChat while busy cancels and clears", c->getState() == Controller::IDLE && c->getEntries().empty() && c->getMessages().empty());
				ck("send after busy newChat", c->send("hi", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("fresh conversation", c->getEntries().size() == 2 && c->getMessages().size() == 2 && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


struct SelectionScenario : MockScenario {
	int64_t moduleId = -1;

	const char* name() const override {
		return "attach-selection";
	}
	bool step() override {
		app::RackWidget* rack = APP->scene->rack;
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				moduleId = addModuleDirect("Fundamental", "VCO");
				app::ModuleWidget* mw = rack->getModule(moduleId);
				ck("module added", mw != NULL);
				if (!mw)
					return true;
				rack->select(mw, true);
				ck("module selected", rack->hasSelection());
				ck("send accepted", c->send("hello", true));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				std::string content = c->getMessages().empty() ? "" : c->getMessages()[0].content;
				ck("USER message has the selection block", has(content, "[Selected modules]"), content);
				ck("block names the module id", has(content, string::f("id %lld:", (long long) moduleId)) && has(content, "Fundamental/VCO"), content);
				ck("block keeps the user text first", content.find("hello") == 0);
				const ChatEntry* e = findEntryWith(c, ChatEntry::USER, "hello");
				ck("USER entry shows only the text and a note", e && !has(e->text, "[Selected modules]") && has(e->text, "(+1 selected module attached)"), e ? e->text : "");
				ck("send without attachment", c->send("second", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				std::string last;
				for (const ChatMessage& m : c->getMessages()) {
					if (m.role == ChatMessage::USER)
						last = m.content;
				}
				ck("no block when attachSelection is false", last == "second", last);
				rack->deselectAll();
				ck("send with attachment but nothing selected", c->send("third", true));
				next();
			} break;
			case 3: {
				if (!idle())
					return false;
				std::string last;
				for (const ChatMessage& m : c->getMessages()) {
					if (m.role == ChatMessage::USER)
						last = m.content;
				}
				ck("no block when nothing is selected", last == "third", last);
				return true;
			}
		}
		return false;
	}
};


struct TrimScenario : MockScenario {
	const char* name() const override {
		return "trim-context";
	}
	void configure(Config& cfg) override {
		cfg.maxContextChars = 1000;
	}
	void init() override {
		useScript([](const ClientOptions&, const ChatRequest&, int) -> ChatResponse {
			return parseResponse(200, chatBody("ok", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("first", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("single turn is never dropped", probe->lastRequestSize == 2, std::to_string(probe->lastRequestSize.load()));
				ck("second send accepted", c->send("second", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("oldest turn dropped from the request (system + latest user)", probe->lastRequestSize == 2, std::to_string(probe->lastRequestSize.load()));
				ck("conversation itself is kept", c->getMessages().size() == 4);
				ck("INFO about trimming", findEntryWith(c, ChatEntry::INFO, "Older messages") != NULL, dumpEntries(c));
				bool systemFirst, hasTools;
				{
					std::lock_guard<std::mutex> lock(probe->mutex);
					systemFirst = !probe->first.messages.empty() && probe->first.messages[0].role == ChatMessage::SYSTEM && has(probe->first.messages[0].content, "built-in assistant") && has(probe->first.messages[0].content, "# Environment");
					hasTools = has(probe->first.toolsJson, "get_patch") && has(probe->first.toolsJson, "clear_patch");
				}
				ck("request starts with the system prompt + environment", systemFirst);
				ck("request carries the tool definitions", hasTools);
				return true;
			}
		}
		return false;
	}
};


struct ConfigScenario : MockScenario {
	const char* name() const override {
		return "config";
	}
	void init() override {
		useScript([](const ClientOptions&, const ChatRequest&, int) -> ChatResponse {
			return parseResponse(200, chatBody("ok", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				Config cfg = c->getConfig();
				cfg.maxToolRounds = 1000;
				std::string err;
				ck("setConfig saves", c->setConfig(cfg, &err), err);
				ck("setConfig applies clamped values", c->getConfig().maxToolRounds == 100);
				Config disk = loadConfig(configPath());
				ck("setConfig wrote assistant.json", disk.maxToolRounds == 100 && disk.mock);

				// External edit applies at the next send
				Config ext = c->getConfig();
				ext.model = "selftest/external-model";
				saveConfig(ext, configPath());
				ck("send accepted", c->send("hello", false));
				ck("send reloads assistant.json", c->getConfig().model == "selftest/external-model", c->getConfig().model);
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				std::string model;
				{
					std::lock_guard<std::mutex> lock(probe->mutex);
					model = probe->lastModel;
				}
				ck("client created with the reloaded config", model == "selftest/external-model", model);

				// A broken file keeps the current config and tells the user
				{
					std::ofstream f(configPath().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
					f << "{not json";
				}
				ck("send accepted", c->send("again", false));
				ck("broken file keeps the previous settings", c->getConfig().model == "selftest/external-model");
				ck("INFO about the broken file", findEntryWith(c, ChatEntry::INFO, "Keeping the previous settings") != NULL, dumpEntries(c));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("run with a broken file still works", countKind(c, ChatEntry::ASSISTANT) == 2 && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


// ---- Scripted run scenarios (history / patch replacement) ----------------------------

/** Shared between a scripted client (worker thread) and its scenario (UI thread). */
struct GateState {
	/** The script blocks in its gated round until gate >= 1 */
	std::atomic<int> gate;
	/** Set by the script while it blocks */
	std::atomic<int> waiting;
	GateState() : gate(0), waiting(0) {}
};

std::string jsonEscapeForSelfTest(const std::string& s) {
	std::string o;
	for (char ch : s) {
		if (ch == '"' || ch == '\\')
			o += '\\';
		o += ch;
	}
	return o;
}

/** Chat completion body with one tool call. `argsJson` is the raw arguments object text. */
std::string toolCallBody(const std::string& id, const std::string& name, const std::string& argsJson) {
	return std::string("{\"model\":\"selftest\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"") + id
		+ "\",\"type\":\"function\",\"function\":{\"name\":\"" + name + "\",\"arguments\":\"" + jsonEscapeForSelfTest(argsJson)
		+ "\"}}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";
}

std::string addModuleArgs(const std::string& model) {
	return "{\"plugin\":\"Fundamental\",\"model\":\"" + model + "\"}";
}

/** Text of the last USER message and the number of TOOL results after it (= the round of the run). */
void scriptPosition(const ChatRequest& req, std::string* tag, int* round) {
	*tag = "";
	*round = 0;
	for (size_t i = req.messages.size(); i > 0; i--) {
		const ChatMessage& m = req.messages[i - 1];
		if (m.role == ChatMessage::USER) {
			*tag = m.content;
			return;
		}
		if (m.role == ChatMessage::TOOL)
			(*round)++;
	}
}

void waitGate(const std::shared_ptr<GateState>& g) {
	g->waiting = 1;
	for (int i = 0; i < 30000 && g->gate.load() < 1; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

struct GatedScenario : MockScenario {
	std::shared_ptr<GateState> gs = std::make_shared<GateState>();
	int baseIndex = 0;

	~GatedScenario() override {
		// Never leave the worker blocked
		gs->gate = 1000;
	}
	/** Blocks the next gated round again */
	void arm() {
		gs->waiting = 0;
		gs->gate = 0;
	}
	void release() {
		gs->gate = 1;
	}
	bool blocked() const {
		return gs->waiting.load() == 1 && c->getState() == Controller::WAITING_HTTP;
	}
};


/** A model that keeps repeating failing calls: identical failures of a round are merged into one "xN" action and the run
stops after MAX_FAILED_ROUNDS rounds without a single successful call. */
struct FailLoopScenario : MockScenario {
	const char* name() const override {
		return "failloop";
	}
	void init() override {
		useScript([](const ClientOptions&, const ChatRequest&, int) -> ChatResponse {
			// Two identical failing calls per round (set_param on a module that does not exist)
			std::string call = "{\"type\":\"function\",\"function\":{\"name\":\"set_param\",\"arguments\":\"{\\\"module_id\\\":-5,\\\"param_id\\\":0,\\\"value\\\":1}\"}}";
			std::string body = "{\"model\":\"selftest\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"a\",";
			body += call.substr(1) + ",{\"id\":\"b\"," + call.substr(1) + "]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";
			return parseResponse(200, body);
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				ck("send accepted", c->send("go", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("stopped by the failed-rounds guard", findEntryWith(c, ChatEntry::INFO, "Stopped: the last 3 tool rounds failed") != NULL, dumpEntries(c));
				ck("exactly 3 model rounds", countRole(c, ChatMessage::ASSISTANT) == 3, std::to_string(countRole(c, ChatMessage::ASSISTANT)));
				ck("API history is valid", historyValid(c->getMessages()));
				bool merged = false;
				for (const ChatEntry& e : c->getEntries()) {
					if (e.kind == ChatEntry::ACTIONS && e.actions.size() == 1 && e.actions[0].repeat == 2 && has(e.actions[0].text, "\xc3\x97" "2"))
						merged = true;
				}
				ck("identical failures of a round are merged", merged, dumpEntries(c));
				return true;
			}
		}
		return false;
	}
};


/** A user edit that happens while the run waits for the model must end up after the run's partial action in the history,
and the next mutation of the run must go into a NEW action after it. */
struct HistoryOrderScenario : GatedScenario {
	const char* name() const override {
		return "history-order";
	}
	void init() override {
		std::shared_ptr<GateState> g = gs;
		useScript([g](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			waitGate(g);
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				baseIndex = APP->history->actionIndex;
				ck("send accepted", c->send("go", false));
				next();
			} break;
			case 1: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added", countModels("Fundamental", "VCO") == 1);
				ck("the run's action is already in the history while the run waits", APP->history->actionIndex == baseIndex + 1 && APP->history->getUndoName() == "assistant changes", APP->history->getUndoName());
				ck("user edit during the run", addModuleDirect("Fundamental", "LFO") >= 0);
				ck("user edit is on top of the run's action", APP->history->actionIndex == baseIndex + 2 && APP->history->getUndoName() == "selftest add");
				release();
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("run finished without errors", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("history order is chronological", APP->history->actionIndex == baseIndex + 2 && (int) APP->history->actions.size() == baseIndex + 2
					&& APP->history->actions[baseIndex]->name == "assistant changes" && APP->history->actions[baseIndex + 1]->name == "selftest add");
				APP->history->undo();
				ck("first undo removes the user's module only", countModels("Fundamental", "LFO") == 0 && countModels("Fundamental", "VCO") == 1);
				APP->history->undo();
				ck("second undo removes the run's module", countModels("Fundamental", "VCO") == 0);
				APP->history->redo();
				APP->history->redo();
				ck("redo restores both", countModels("Fundamental", "LFO") == 1 && countModels("Fundamental", "VCO") == 1);
				return true;
			}
		}
		return false;
	}
};


/** Undo while the run waits reverts the run's partial changes; later tool calls start a new action without touching freed memory. */
struct UndoDuringRunScenario : GatedScenario {
	const char* name() const override {
		return "undo-during-run";
	}
	void init() override {
		std::shared_ptr<GateState> g = gs;
		useScript([g](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			if (round == 1) {
				waitGate(g);
				return parseResponse(200, toolCallBody("c1", "add_module", addModuleArgs("VCF")));
			}
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				baseIndex = APP->history->actionIndex;
				ck("send accepted", c->send("go", false));
				next();
			} break;
			case 1: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added", countModels("Fundamental", "VCO") == 1);
				APP->history->undo();
				ck("undo during the run reverts the run's changes", countModels("Fundamental", "VCO") == 0 && moduleCount() == 0);
				release();
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("run finished without errors", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("later tool call still ran", countModels("Fundamental", "VCF") == 1 && countModels("Fundamental", "VCO") == 0);
				ck("new action replaced the undone one", APP->history->actionIndex == baseIndex + 1 && (int) APP->history->actions.size() == baseIndex + 1
					&& APP->history->getUndoName() == "assistant changes");
				APP->history->undo();
				ck("undo reverts the second part", moduleCount() == 0);
				APP->history->redo();
				ck("redo brings it back", countModels("Fundamental", "VCF") == 1);
				return true;
			}
		}
		return false;
	}
};


/** New/Open/Revert during a run cancels it and keeps the run's old actions out of the new patch's history. */
struct PatchReplacedScenario : GatedScenario {
	const char* name() const override {
		return "patch-replaced";
	}
	void init() override {
		std::shared_ptr<GateState> g = gs;
		useScript([g](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			if (has(tag, "confirm")) {
				if (round == 1)
					return parseResponse(200, toolCallBody("c1", "clear_patch", "{}"));
				return parseResponse(200, chatBody("done", "stop"));
			}
			if (round == 1) {
				waitGate(g);
				return parseResponse(200, toolCallBody("c1", "add_module", addModuleArgs("VCF")));
			}
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("send accepted", c->send("go", false));
				next();
			} break;
			case 1: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added", countModels("Fundamental", "VCO") == 1 && APP->history->canUndo());
				APP->patch->clear();
				ck("patch cleared", moduleCount() == 0 && !APP->history->canUndo());
				release();
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("run was cancelled because of the new patch", findEntryWith(c, ChatEntry::INFO, "the patch was replaced") != NULL, dumpEntries(c));
				ck("the model's late answer was not executed", moduleCount() == 0);
				ck("the new patch has an empty history", !APP->history->canUndo() && !APP->history->canRedo());
				ck("API history is valid", historyValid(c->getMessages()));
				ck("send works afterwards", c->send("confirm", false));
				next();
			} break;
			case 3: {
				if (c->getState() != Controller::WAITING_CONFIRM) {
					if (idle()) {
						ck("reached WAITING_CONFIRM", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("clear_patch asks for confirmation", ce && ce->confirmState == ChatEntry::PENDING, dumpEntries(c));
				APP->patch->clear();
				int64_t lfo = addModuleDirect("Fundamental", "LFO");
				ck("another patch with a module", lfo >= 0 && moduleCount() == 1);
				if (ce)
					c->confirm(ce->id, true);
				ck("a stale answer does not resume the run", c->getState() == Controller::WAITING_CONFIRM);
				next();
			} break;
			case 4: {
				if (!idle())
					return false;
				const ChatEntry* ce = lastOfKind(c, ChatEntry::CONFIRM);
				ck("CONFIRM entry is CANCELLED", ce && ce->confirmState == ChatEntry::CANCELLED);
				ck("the stale clear_patch did not run", countModels("Fundamental", "LFO") == 1 && moduleCount() == 1);
				ck("history holds only the new patch's own action", APP->history->actions.size() == 1 && APP->history->getUndoName() == "selftest add");
				ck("API history is valid", historyValid(c->getMessages()));
				return true;
			}
		}
		return false;
	}
};


/** The patch counts as saved only if it matches the saved file. */
struct SavedStateScenario : GatedScenario {
	std::string dir = "assistant-selftest-saved";
	std::string file;

	const char* name() const override {
		return "saved-state";
	}
	void init() override {
		file = system::join(asset::user("patches"), dir + "/saved.vcv");
		std::shared_ptr<GateState> g = gs;
		std::string path = dir + "/saved.vcv";
		useScript([g, path](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			if (round == 1)
				return parseResponse(200, toolCallBody("c1", "save_patch", "{\"path\":\"" + path + "\"}"));
			if (has(tag, "more")) {
				// save, then change the patch again
				if (round == 2)
					return parseResponse(200, toolCallBody("c2", "add_module", addModuleArgs("VCF")));
			}
			else if (has(tag, "wait")) {
				if (round == 2) {
					waitGate(g);
					return parseResponse(200, chatBody("done", "stop"));
				}
			}
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	~SavedStateScenario() override {
		system::removeRecursively(system::join(asset::user("patches"), dir));
		APP->patch->path = "";
	}
	bool step() override {
		switch (phase) {
			case 0: {
				prepareEmptyPatch();
				ck("send accepted", c->send("plain", false));
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("save_patch worked", system::exists(file) && countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("patch is saved after add + save", APP->history->isSaved());
				ck("send accepted", c->send("wait", false));
				next();
			} break;
			case 2: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("saved while the run waits", APP->history->isSaved());
				ck("user edit during the run", addModuleDirect("Fundamental", "LFO") >= 0);
				release();
				next();
			} break;
			case 3: {
				if (!idle())
					return false;
				ck("a user edit after save_patch leaves the patch unsaved", !APP->history->isSaved());
				arm();
				ck("send accepted", c->send("more", false));
				next();
			} break;
			case 4: {
				if (!idle())
					return false;
				ck("mutation after save_patch leaves the patch unsaved", !APP->history->isSaved(), dumpEntries(c));
				APP->history->undo();
				ck("undoing the run does not claim the patch is saved", !APP->history->isSaved());
				APP->history->redo();
				return true;
			}
		}
		return false;
	}
};


/** Edge cases of the run's undo action while the run waits: redo of the run's action and the 500-action history cap. */
struct HistoryEdgeScenario : GatedScenario {
	const char* name() const override {
		return "history-edge";
	}
	void init() override {
		std::shared_ptr<GateState> g = gs;
		useScript([g](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			if (round == 1) {
				waitGate(g);
				return parseResponse(200, toolCallBody("c1", "add_module", addModuleArgs("VCF")));
			}
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	static void pushFillers(int n) {
		for (int i = 0; i < n; i++) {
			history::ComplexAction* a = new history::ComplexAction;
			a->name = "selftest filler";
			APP->history->push(a);
		}
	}
	bool step() override {
		switch (phase) {
			case 0: {
				APP->patch->clear();
				prepareEmptyPatch();
				baseIndex = APP->history->actionIndex;
				ck("send accepted", c->send("redo", false));
				next();
			} break;
			case 1: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				APP->history->undo();
				ck("user undid the run's action", countModels("Fundamental", "VCO") == 0);
				next();
			} break;
			case 2: {
				// One frame later the controller has noticed the undo
				if (phaseFrames++ < 2)
					return false;
				APP->history->redo();
				ck("user redid the run's action", countModels("Fundamental", "VCO") == 1);
				release();
				next();
			} break;
			case 3: {
				if (!idle())
					return false;
				ck("run finished without errors", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("both modules exist", countModels("Fundamental", "VCO") == 1 && countModels("Fundamental", "VCF") == 1);
				ck("history is consistent (no stale pointers)", APP->history->actionIndex == (int) APP->history->actions.size() && APP->history->actionIndex >= baseIndex + 1);
				APP->history->undo();
				APP->history->undo();
				APP->history->redo();
				APP->history->redo();
				ck("undo/redo still works", countModels("Fundamental", "VCO") == 1 && countModels("Fundamental", "VCF") == 1);
				arm();
				ck("send accepted", c->send("cap", false));
				next();
			} break;
			case 4: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added by the second run", countModels("Fundamental", "VCO") == 2);
				// 500 user actions push the run's action out of the history
				pushFillers(500);
				ck("history is capped at 500", APP->history->actions.size() == 500);
				release();
				next();
			} break;
			case 5: {
				if (!idle())
					return false;
				ck("run finished without errors", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("later mutation ran after the cap trimmed the run's action", countModels("Fundamental", "VCF") == 2);
				ck("it is in a new action at the top", APP->history->actionIndex == 500 && APP->history->getUndoName() == "assistant changes");
				ck("history stayed capped", APP->history->actions.size() == 500);
				// The run's first action was trimmed away, so one undo step cannot revert the whole run: no partial "Undo changes"
				ck("a run split by the history cap offers no undoLastRun", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun is a no-op for a split run", countModels("Fundamental", "VCF") == 2 && countModels("Fundamental", "VCO") == 2);
				APP->patch->clear();
				return true;
			}
		}
		return false;
	}
};


/** canUndoLastRun() / undoLastRun() semantics. */
struct UndoLastRunScenario : GatedScenario {
	const char* name() const override {
		return "undo-last-run";
	}
	void init() override {
		std::shared_ptr<GateState> g = gs;
		useScript([g](const ClientOptions&, const ChatRequest& req, int) -> ChatResponse {
			std::string tag;
			int round;
			scriptPosition(req, &tag, &round);
			if (tag == "text")
				return parseResponse(200, chatBody("just text", "stop"));
			if (round == 0)
				return parseResponse(200, toolCallBody("c0", "add_module", addModuleArgs("VCO")));
			if (tag == "two" && round == 1)
				return parseResponse(200, toolCallBody("c1", "add_module", addModuleArgs("VCF")));
			if (tag == "gsplit" && round == 1) {
				waitGate(g);
				return parseResponse(200, toolCallBody("c1", "add_module", addModuleArgs("VCF")));
			}
			if (tag == "gate" && round == 1)
				waitGate(g);
			return parseResponse(200, chatBody("done", "stop"));
		});
	}
	bool step() override {
		switch (phase) {
			case 0: {
				// The previous scenario left a full (500 actions) history behind
				APP->patch->clear();
				prepareEmptyPatch();
				baseIndex = APP->history->actionIndex;
				ck("nothing to undo before any run", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun without a run is a no-op", APP->history->actionIndex == baseIndex);
				ck("send accepted", c->send("one", false));
				ck("false while busy", !c->canUndoLastRun());
				next();
			} break;
			case 1: {
				if (!idle())
					return false;
				ck("VCO added", countModels("Fundamental", "VCO") == 1);
				ck("can undo the finished run", c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun reverted the run in one step", moduleCount() == 0 && APP->history->actionIndex == baseIndex, string::f("modules %d idx %d base %d", (int) moduleCount(), APP->history->actionIndex, baseIndex));
				ck("the button state is gone after the undo", !c->canUndoLastRun());
				c->undoLastRun();
				ck("a second undoLastRun is a no-op", APP->history->actionIndex == baseIndex && moduleCount() == 0);
				ck("send accepted", c->send("two", false));
				next();
			} break;
			case 2: {
				if (!idle())
					return false;
				ck("two modules added by one run", countModels("Fundamental", "VCO") == 1 && countModels("Fundamental", "VCF") == 1 && APP->history->actionIndex == baseIndex + 1);
				ck("can undo", c->canUndoLastRun());
				ck("user edit after the run", addModuleDirect("Fundamental", "LFO") >= 0);
				ck("a user edit disables undoLastRun", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun does nothing then", moduleCount() == 3);
				APP->history->undo();
				ck("user undid own edit", moduleCount() == 2);
				ck("run's action is the newest undo step again", c->canUndoLastRun());
				c->undoLastRun();
				ck("whole run reverted", moduleCount() == 0 && APP->history->actionIndex == baseIndex);
				ck("send accepted", c->send("one", false));
				next();
			} break;
			case 3: {
				if (!idle())
					return false;
				ck("run added a module", moduleCount() == 1 && c->canUndoLastRun());
				ck("send accepted", c->send("text", false));
				next();
			} break;
			case 4: {
				if (!idle())
					return false;
				ck("a later run without changes forgets the earlier one", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun is a no-op", moduleCount() == 1);
				arm();
				ck("send accepted", c->send("gate", false));
				next();
			} break;
			case 5: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added", moduleCount() == 2);
				ck("busy: cannot undo", !c->canUndoLastRun());
				c->undoLastRun();
				ck("busy: undoLastRun is a no-op", moduleCount() == 2);
				release();
				next();
			} break;
			case 6: {
				if (!idle())
					return false;
				ck("can undo after the gated run", c->canUndoLastRun());
				APP->patch->clear();
				ck("patch cleared", moduleCount() == 0);
				ck("a cleared patch disables undoLastRun", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun after clear is safe", moduleCount() == 0 && !APP->history->canUndo());
				ck("send accepted", c->send("one", false));
				next();
			} break;
			case 7: {
				if (!idle())
					return false;
				ck("run in the new patch", moduleCount() == 1 && c->canUndoLastRun());
				c->undoLastRun();
				ck("undo works in the new patch", moduleCount() == 0);
				ck("send accepted", c->send("one", false));
				next();
			} break;
			case 8: {
				if (!idle())
					return false;
				ck("can undo", c->canUndoLastRun());
				// Replace the run's action by an unrelated one of the same history position (new allocation)
				APP->history->undo();
				ck("user undo", moduleCount() == 0 && !c->canUndoLastRun());
				history::ComplexAction* a = new history::ComplexAction;
				a->name = "selftest other";
				APP->history->push(a);
				ck("a new action at the same history position is not the run's action", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun leaves it alone", APP->history->getUndoName() == "selftest other" && APP->history->actionIndex == 1);
				APP->patch->clear();
				prepareEmptyPatch();
				arm();
				ck("send accepted", c->send("gsplit", false));
				next();
			} break;
			case 9: {
				if (!blocked()) {
					if (idle()) {
						ck("run reached the gated round", false, dumpEntries(c));
						return true;
					}
					return false;
				}
				ck("VCO added", countModels("Fundamental", "VCO") == 1);
				// The user edits the patch between two mutating calls of the run: the run ends up with two undo steps
				ck("user edit while the run waits", addModuleDirect("Fundamental", "LFO") >= 0);
				release();
				next();
			} break;
			case 10: {
				if (!idle())
					return false;
				ck("run finished without errors", countKind(c, ChatEntry::ERROR) == 0, dumpEntries(c));
				ck("all three modules exist", moduleCount() == 3 && countModels("Fundamental", "VCF") == 1);
				ck("a run split by a user edit offers no partial undo", !c->canUndoLastRun());
				c->undoLastRun();
				ck("undoLastRun is a no-op for a split run", moduleCount() == 3);
				APP->patch->clear();
				return true;
			}
		}
		return false;
	}
};


/** While the modal settings dialog is open, no key may reach the Scene (undo, redo, delete selection, open, new, ...). Keys are injected through the real event dispatch. */
struct SettingsKeysScenario : MockScenario {
	size_t sceneChildren = 0;
	int historyIndex = 0;
	size_t modules = 0;

	const char* name() const override {
		return "settings-keys";
	}
	static void key(int k, int mods) {
		math::Vec pos = APP->scene->box.size.div(2.f);
		APP->event->handleKey(pos, k, 0, GLFW_PRESS, mods);
		APP->event->handleKey(pos, k, 0, GLFW_RELEASE, mods);
	}
	bool step() override {
		switch (phase) {
			case 0: {
				APP->patch->clear();
				int64_t id = addModuleDirect("Fundamental", "VCO");
				ck("module added", id >= 0 && moduleCount() == 1);
				APP->scene->rack->selectAll();
				ck("module selected", APP->scene->rack->hasSelection());
				modules = moduleCount();
				historyIndex = APP->history->actionIndex;
				sceneChildren = APP->scene->children.size();
				showSettingsDialog(c);
				ck("dialog was added to the scene", APP->scene->children.size() == sceneChildren + 1);
				ck("a field has the keyboard focus", APP->event->selectedWidget != NULL);
				next();
			} break;
			case 1: {
				if (phaseFrames++ < 3)
					return false;
				// Keys with a text field focused
				key(GLFW_KEY_Z, GLFW_MOD_CONTROL);
				key(GLFW_KEY_Z, GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
				key(GLFW_KEY_Y, GLFW_MOD_CONTROL);
				key(GLFW_KEY_BACKSPACE, 0);
				key(GLFW_KEY_DELETE, 0);
				key(GLFW_KEY_D, GLFW_MOD_CONTROL);
				key(GLFW_KEY_E, GLFW_MOD_CONTROL);
				key(GLFW_KEY_F11, 0);
				key(GLFW_KEY_L, GLFW_MOD_CONTROL);
				ck("the patch is untouched by keys in a field", moduleCount() == modules && APP->history->actionIndex == historyIndex);
				ck("the panel was not toggled by Ctrl+L", !isPanelHiddenToggled());
				// Keys when the focus is on a widget that does not handle keys: clicks move it to the dialog
				APP->event->setSelectedWidget(NULL);
				next();
			} break;
			case 2: {
				if (phaseFrames++ < 3)
					return false;
				ck("focus returns into the dialog", APP->event->selectedWidget != NULL);
				key(GLFW_KEY_Z, GLFW_MOD_CONTROL);
				key(GLFW_KEY_BACKSPACE, 0);
				key(GLFW_KEY_DELETE, 0);
				ck("the patch is untouched by keys on the dialog", moduleCount() == modules && APP->history->actionIndex == historyIndex);
				// Escape closes the dialog
				key(GLFW_KEY_ESCAPE, 0);
				next();
			} break;
			case 3: {
				if (phaseFrames++ < 3)
					return false;
				ck("Escape closed the dialog", APP->scene->children.size() == sceneChildren);
				ck("the patch is still untouched", moduleCount() == modules && APP->history->actionIndex == historyIndex);
				// Without the dialog the same shortcut works again
				APP->scene->rack->deselectAll();
				key(GLFW_KEY_Z, GLFW_MOD_CONTROL);
				ck("Ctrl+Z works after the dialog is closed", moduleCount() == 0);
				return true;
			}
		}
		return false;
	}
	bool isPanelHiddenToggled() {
		// The panel is hidden in the selftest run and must stay so
		return isPanelVisible();
	}
};


struct MockRunner {
	typedef MockScenario* (*Factory)();

	std::vector<Factory> factories;
	size_t index = 0;
	std::unique_ptr<MockScenario> current;
	std::unique_ptr<Controller> controller;
	std::chrono::steady_clock::time_point scenarioStart;
	FileBackup configBackup;
	FileBackup promptBackup;
	bool started = false;

	template <class T>
	static MockScenario* make() {
		return new T;
	}

	MockRunner() {
		factories.push_back(&make<PromptScenario>);
		factories.push_back(&make<BuildScenario>);
		factories.push_back(&make<DeleteScenario>);
		factories.push_back(&make<DeleteNoConfirmScenario>);
		factories.push_back(&make<BadArgsScenario>);
		factories.push_back(&make<ErrorScenario>);
		factories.push_back(&make<CancelScenario>);
		factories.push_back(&make<CancelConfirmScenario>);
		factories.push_back(&make<LoopScenario>);
		factories.push_back(&make<RetryScenario>);
		factories.push_back(&make<LengthScenario>);
		factories.push_back(&make<EmptyReplyScenario>);
		factories.push_back(&make<NewChatScenario>);
		factories.push_back(&make<SelectionScenario>);
		factories.push_back(&make<TrimScenario>);
		factories.push_back(&make<ConfigScenario>);
		factories.push_back(&make<FailLoopScenario>);
		factories.push_back(&make<HistoryOrderScenario>);
		factories.push_back(&make<UndoDuringRunScenario>);
		factories.push_back(&make<PatchReplacedScenario>);
		factories.push_back(&make<SavedStateScenario>);
		factories.push_back(&make<HistoryEdgeScenario>);
		factories.push_back(&make<UndoLastRunScenario>);
		factories.push_back(&make<SettingsKeysScenario>);
	}

	~MockRunner() {
		finishAll();
	}

	void finishAll() {
		current.reset();
		controller.reset();
		if (started) {
			configBackup.restore();
			promptBackup.restore();
			started = false;
		}
	}

	/** Called once per frame. Returns true when all scenarios are done. */
	bool step(SelfTestRun& t) {
		if (!started) {
			// The selftest must never clobber a real assistant.json / prompt file
			configBackup.backup(configPath());
			promptBackup.backup(systemPromptPath());
			started = true;
		}
		if (!current) {
			if (index >= factories.size()) {
				finishAll();
				return true;
			}
			current.reset(factories[index]());
			INFO("[assistant selftest] scenario mock/%s", current->name());
			controller.reset(new Controller());
			Config cfg = defaultConfig();
			cfg.mock = true;
			cfg.confirmDestructive = true;
			cfg.apiKey.clear();
			cfg.attachSelection = true;
			current->configure(cfg);
			controller->setConfig(cfg);
			controller->setClientFactory([](const ClientOptions& o) -> std::shared_ptr<LlmClient> {
				ClientOptions mo = o;
				// Never depend on a user's scripted responses
				mo.mockScriptPath = "";
				return createMockClient(mo);
			});
			current->t = &t;
			current->c = controller.get();
			scenarioStart = std::chrono::steady_clock::now();
			try {
				current->init();
			}
			catch (const std::exception& e) {
				t.check(std::string("mock/") + current->name() + " init", false, e.what());
			}
		}

		bool done = false;
		controller->step();
		current->phaseFrames++;
		try {
			done = current->step();
		}
		catch (const std::exception& e) {
			t.check(std::string("mock/") + current->name() + " completed", false, std::string("exception: ") + e.what());
			done = true;
		}
		if (!done) {
			double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - scenarioStart).count();
			if (sec > MOCK_SCENARIO_TIMEOUT_SEC) {
				t.check(std::string("mock/") + current->name() + " finished in time", false, string::f("phase %d, state %d, entries: ", current->phase, (int) controller->getState()) + dumpEntries(controller.get()));
				done = true;
			}
		}
		if (done) {
			controller->cancel();
			current.reset();
			controller.reset();
			index++;
		}
		return false;
	}
};


// ---- Driver --------------------------------------------------------------------------

typedef void (*ScenarioFn)(SelfTestRun& t);

struct ScenarioEntry {
	const char* name;
	ScenarioFn fn;
};


/** Synchronous scenarios selected by RACK_ASSISTANT_SELFTEST ("tools", "mock" or "all").
The step-driven "mock" controller scenarios run afterwards, see MockRunner. */
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
	// state: 0 = not initialized, 1 = waiting for frames, 2 = done / disabled, 3 = running the mock scenarios
	static int state = 0;
	static int frames = 0;
	static std::string mode;
	static SelfTestRun run;
	static std::unique_ptr<MockRunner> mockRunner;

	if (state == 2)
		return;
	if (state == 3) {
		bool done = false;
		try {
			done = mockRunner->step(run);
		}
		catch (const std::exception& e) {
			run.check("mock scenarios completed", false, std::string("exception: ") + e.what());
			done = true;
		}
		if (done) {
			mockRunner.reset();
			state = 2;
			finish(run);
		}
		return;
	}
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
	if (mode == "mock" || mode == "all") {
		mockRunner.reset(new MockRunner);
		state = 3;
	}
	else {
		state = 2;
		finish(run);
	}
}


} // namespace assistant
} // namespace rack

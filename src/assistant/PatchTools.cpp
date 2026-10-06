#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <asset.hpp>
#include <color.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <patch.hpp>
#include <plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>


namespace rack {
namespace assistant {


using namespace helpers;


// ---- Small argument utilities --------------------------------------------------------

/** Reads an optional boolean. Returns false (with err) if present but invalid. */
static bool optBool(json_t* args, const char* key, bool def, bool* out, std::string* err) {
	*out = def;
	json_t* j = json_object_get(args, key);
	if (!j || json_is_null(j))
		return true;
	if (!argBool(args, key, out)) {
		*err = string::f("Invalid '%s': expected true or false.", key);
		return false;
	}
	return true;
}


static std::string joinIds(const std::vector<int64_t>& ids) {
	std::string s;
	for (int64_t id : ids)
		s += (s.empty() ? "" : ", ") + std::to_string((long long) id);
	return s;
}


static json_t* portEndJson(engine::Module* module, bool input, int idx) {
	json_t* o = json_object();
	setInt(o, "module", module->id);
	setInt(o, input ? "input" : "output", idx);
	setStr(o, "name", portName(module, input, idx));
	return o;
}


static json_t* cableJson(app::CableWidget* cw) {
	json_t* o = json_object();
	setInt(o, "id", cw->cable->id);
	setStr(o, "color", color::toHexString(cw->color));
	setJson(o, "from", portEndJson(cw->cable->outputModule, false, cw->cable->outputId));
	setJson(o, "to", portEndJson(cw->cable->inputModule, true, cw->cable->inputId));
	return o;
}


static std::string cableLabel(app::CableWidget* cw) {
	return endLabel(cw->cable->outputModule, false, cw->cable->outputId) + " \xE2\x86\x92 " + endLabel(cw->cable->inputModule, true, cw->cable->inputId);
}


/** Complete, engine-backed cables sorted by id. */
static std::vector<app::CableWidget*> sortedCables() {
	std::vector<app::CableWidget*> out;
	for (app::CableWidget* cw : APP->scene->rack->getCompleteCables()) {
		if (cw->cable && cw->cable->inputModule && cw->cable->outputModule)
			out.push_back(cw);
	}
	std::sort(out.begin(), out.end(), [](app::CableWidget* a, app::CableWidget* b) {
		return a->cable->id < b->cable->id;
	});
	return out;
}


// ---- get_patch -----------------------------------------------------------------------

static ToolResult getPatch(json_t* args, ToolContext& ctx) {
	std::string err;
	bool includeAll = false;
	if (!optBool(args, "include_all_params", false, &includeAll, &err))
		return errorResult(err);

	// Optional module filter
	bool filtered = false;
	std::set<int64_t> filter;
	json_t* idsJ = json_object_get(args, "module_ids");
	if (idsJ && !json_is_null(idsJ)) {
		if (!json_is_array(idsJ))
			return errorResult("Invalid 'module_ids': expected an array of module ids.");
		filtered = true;
		size_t i;
		json_t* el;
		json_array_foreach(idsJ, i, el) {
			JsonPtr tmp(json_pack("{s:O}", "v", el));
			int64_t id;
			if (!argInt64(tmp.get(), "v", &id))
				return errorResult("Invalid 'module_ids': every entry must be an integer module id.");
			filter.insert(id);
		}
	}

	app::RackWidget* rack = APP->scene->rack;
	std::vector<app::ModuleWidget*> mws = rack->getModules();
	struct Item {
		app::ModuleWidget* mw;
		GridPos pos;
	};
	std::vector<Item> items;
	for (app::ModuleWidget* mw : mws) {
		if (!mw->module)
			continue;
		if (filtered && !filter.count(mw->module->id))
			continue;
		Item it;
		it.mw = mw;
		it.pos = gridPosOf(mw);
		items.push_back(it);
	}
	std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
		if (a.pos.row != b.pos.row)
			return a.pos.row < b.pos.row;
		return a.pos.hp < b.pos.hp;
	});

	json_t* root = json_object();
	JsonPtr guard(root);
	if (APP->patch->path.empty())
		setJson(root, "path", json_null());
	else
		setStr(root, "path", APP->patch->path);
	setBool(root, "unsaved", !APP->history->isSaved());

	std::vector<app::CableWidget*> cables = sortedCables();
	setInt(root, "module_count", (int64_t) mws.size());
	setInt(root, "cable_count", (int64_t) cables.size());

	json_t* selJ = json_array();
	std::vector<int64_t> selIds;
	for (app::ModuleWidget* mw : rack->getSelected()) {
		if (mw->module)
			selIds.push_back(mw->module->id);
	}
	std::sort(selIds.begin(), selIds.end());
	for (int64_t id : selIds)
		appendJson(selJ, jInt(id));
	setJson(root, "selected_module_ids", selJ);

	json_t* modulesJ = json_array();
	std::set<int64_t> listed;
	for (const Item& it : items) {
		engine::Module* module = it.mw->module;
		listed.insert(module->id);
		json_t* m = json_object();
		setInt(m, "id", module->id);
		plugin::Model* model = module->model;
		setStr(m, "plugin", model && model->plugin ? model->plugin->slug : "");
		setStr(m, "model", model ? model->slug : "");
		setStr(m, "name", model ? model->name : "");
		setJson(m, "pos", posJson(it.pos.hp, it.pos.row));
		setInt(m, "width_hp", widthHp(it.mw));
		setBool(m, "bypassed", module->isBypassed());
		json_t* paramsJ = json_array();
		for (size_t i = 0; i < module->params.size(); i++) {
			engine::ParamQuantity* pq = paramQuantity(module, (int) i);
			if (!pq)
				continue;
			float v = module->params[i].getValue();
			if (!includeAll && std::fabs(v - pq->getDefaultValue()) <= 1e-6f)
				continue;
			json_t* p = json_object();
			setInt(p, "id", (int64_t) i);
			setStr(p, "name", pq->getLabel());
			setNum(p, "value", v);
			setStr(p, "display", displayString(pq));
			appendJson(paramsJ, p);
		}
		setJson(m, "params", paramsJ);
		appendJson(modulesJ, m);
	}
	setJson(root, "modules", modulesJ);

	json_t* cablesJ = json_array();
	for (app::CableWidget* cw : cables) {
		if (filtered && !listed.count(cw->cable->inputModule->id) && !listed.count(cw->cable->outputModule->id))
			continue;
		appendJson(cablesJ, cableJson(cw));
	}
	setJson(root, "cables", cablesJ);

	if (filtered) {
		std::vector<int64_t> unknown;
		for (int64_t id : filter) {
			if (!listed.count(id))
				unknown.push_back(id);
		}
		if (!unknown.empty()) {
			json_t* u = json_array();
			for (int64_t id : unknown)
				appendJson(u, jInt(id));
			setJson(root, "unknown_module_ids", u);
		}
	}

	ToolResult r;
	r.ok = true;
	r.readOnly = true;
	r.content = okJson(guard.release());
	r.summary = string::f("Read patch (%d modules)", (int) mws.size());
	return r;
}


// ---- add_module ----------------------------------------------------------------------

/** Removes a module that was never wrapped in a widget from the engine and deletes it.
	Safe if a throwing widget constructor already removed and deleted the module (the pointer is only compared). */
static void discardModule(engine::Module* module) {
	if (!APP->engine->hasModule(module))
		return;
	try {
		APP->engine->removeModule(module);
	}
	catch (...) {
		// Cannot guarantee the engine let go of the module; leak it rather than risk a dangling pointer.
		if (APP->engine->hasModule(module))
			return;
	}
	delete module;
}


static ToolResult addModule(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);

	std::string pluginSlug, modelSlug;
	if (!argString(args, "plugin", &pluginSlug) || !argString(args, "model", &modelSlug) || trim(pluginSlug).empty() || trim(modelSlug).empty())
		return errorResult("Missing 'plugin' and/or 'model' (strings, the slugs from search_modules).");
	pluginSlug = trim(pluginSlug);
	modelSlug = trim(modelSlug);

	plugin::Model* model = plugin::getModel(pluginSlug, modelSlug);
	if (!model) {
		plugin::Plugin* p = plugin::getPlugin(pluginSlug);
		if (!p)
			return errorResult(string::f("Plugin '%s' not found. Use search_modules to find the exact plugin and model slugs.", pluginSlug.c_str()));
		std::string avail;
		int n = 0;
		for (plugin::Model* m : p->models) {
			if (m->hidden)
				continue;
			if (n++ >= 15)
				break;
			avail += (avail.empty() ? "" : ", ") + m->slug;
		}
		return errorResult(string::f("Model '%s' not found in plugin '%s'. Use search_modules to find the exact model slug. Models of this plugin include: %s", modelSlug.c_str(), pluginSlug.c_str(), avail.c_str()));
	}
	if (model->hidden)
		return errorResult(string::f("Model '%s/%s' is hidden (deprecated) and cannot be added. Use search_modules to find an alternative.", pluginSlug.c_str(), modelSlug.c_str()));

	// Validate placement arguments before creating anything
	bool hasPosition = false;
	int reqHp = 0, reqRow = 0;
	json_t* posJ = json_object_get(args, "position");
	if (posJ && !json_is_null(posJ)) {
		if (!parsePosition(posJ, &reqHp, &reqRow, &err))
			return errorResult(err);
		hasPosition = true;
	}
	ModuleRef nearRef;
	bool hasNear = false;
	json_t* nearJ = json_object_get(args, "near_module_id");
	std::string nearNote;
	if (nearJ && !json_is_null(nearJ)) {
		// near_module_id is only a placement hint: models often send a placeholder (0, -1) or the id
		// of a module added in the same batch that does not exist yet. Do not fail the call for that.
		std::string nearErr;
		if (moduleArg(args, "near_module_id", &nearRef, &nearErr))
			hasNear = true;
		else
			nearNote = "near_module_id does not match a module in the patch and was ignored (omit it to place the module after the previously added one).";
	}

	app::RackWidget* rack = APP->scene->rack;

	// Create module and widget
	engine::Module* module = model->createModule();
	if (!module)
		return errorResult("The plugin failed to create the module.");
	try {
		APP->engine->addModule(module);
	}
	catch (...) {
		// A plugin callback (onAdd, onSampleRateChange) threw; the engine may already hold the module.
		discardModule(module);
		throw;
	}
	app::ModuleWidget* mw = NULL;
	try {
		mw = model->createModuleWidget(module);
	}
	catch (...) {
		// If the widget constructor had already called setModule(), unwinding destroyed the
		// ModuleWidget base, which removed and deleted the module. Only free it if still owned.
		discardModule(module);
		throw;
	}
	if (!mw || mw->module != module) {
		// The widget did not adopt the module; release both separately.
		delete mw;
		APP->engine->removeModule(module);
		delete module;
		return errorResult("The plugin failed to create the module widget.");
	}
	// From here on, `delete mw` also removes the module from the engine and deletes it.

	std::string note;
	GridPos placed;
	try {
		int w = widthHp(mw);
		Occupancy occ;
		if (hasPosition) {
			placed.hp = reqHp;
			placed.row = reqRow;
			if (!occ.isFree(reqHp, reqRow, w)) {
				placed.hp = occ.findFreeHp(reqHp, reqRow, w, false);
				note = string::f("Requested position HP %d, row %d is occupied; placed at HP %d, row %d instead.", reqHp, reqRow, placed.hp, placed.row);
			}
		}
		else {
			// First free slot to the right of an anchor module, else of the row 0 contents
			bool anchored = false;
			app::ModuleWidget* anchor = NULL;
			if (hasNear) {
				anchor = nearRef.mw;
			}
			else {
				for (auto it = ctx.addedModuleIds.rbegin(); it != ctx.addedModuleIds.rend(); ++it) {
					app::ModuleWidget* a = rack->getModule(*it);
					if (a) {
						anchor = a;
						break;
					}
				}
				if (!anchor) {
					// Rightmost selected module
					int bestRight = 0;
					for (app::ModuleWidget* s : rack->getSelected()) {
						GridPos sp = gridPosOf(s);
						int right = sp.hp + widthHp(s);
						if (!anchor || right > bestRight) {
							anchor = s;
							bestRight = right;
						}
					}
				}
			}
			if (anchor) {
				GridPos ap = gridPosOf(anchor);
				placed.row = ap.row;
				placed.hp = occ.findFreeHp(ap.hp + widthHp(anchor), ap.row, w, true);
				anchored = true;
			}
			if (!anchored) {
				placed.row = 0;
				placed.hp = occ.findFreeHp(occ.rowRightEdge(0, 0), 0, w, true);
			}
		}
		mw->setGridPosition(math::Vec((float) placed.hp, (float) placed.row));
		rack->addModule(mw);
	}
	catch (...) {
		delete mw;
		throw;
	}

	try {
		mw->loadTemplate();
	}
	catch (const std::exception&) {
		// A broken module template must not fail the tool
	}

	history::ModuleAdd* ha = new history::ModuleAdd;
	try {
		ha->setModule(mw);
	}
	catch (...) {
		// Roll the module back so the failed call leaves no un-undoable module behind
		delete ha;
		rack->removeModule(mw);
		delete mw;
		throw;
	}
	ctx.undo->push(ha);
	int64_t id = module->id;
	ctx.addedModuleIds.push_back(id);

	json_t* o = json_object();
	setInt(o, "module_id", id);
	setStr(o, "plugin", model->plugin->slug);
	setStr(o, "model", model->slug);
	setStr(o, "name", model->name);
	setJson(o, "pos", posJson(placed.hp, placed.row));
	setInt(o, "width_hp", widthHp(mw));
	if (!nearNote.empty())
		note += (note.empty() ? "" : " ") + nearNote;
	if (!note.empty())
		setStr(o, "note", note);

	ToolResult r;
	r.content = okJson(o);
	r.summary = model->name + " added";
	r.mutated = true;
	return r;
}


// ---- remove_module -------------------------------------------------------------------

static std::string removeModuleConfirmation(json_t* args) {
	ModuleRef m;
	std::string err;
	if (!moduleArg(args, "module_id", &m, &err))
		return "";
	// Module ids are long random numbers; the position identifies the module for the user
	GridPos gp = gridPosOf(m.mw);
	std::string pluginSlug = m.module->model && m.module->model->plugin ? m.module->model->plugin->slug : "";
	int cables = countModuleCables(m);
	return string::f("Remove module '%s' (%s, HP %d, row %d) and its %d cable%s?", moduleName(m).c_str(), pluginSlug.c_str(), gp.hp, gp.row, cables, cables == 1 ? "" : "s");
}


static ToolResult removeModule(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);
	ModuleRef m;
	if (!moduleArg(args, "module_id", &m, &err))
		return errorResult(err);

	std::string name = moduleName(m);
	int64_t id = m.module->id;
	int removedCables = removeModuleWithHistory(m.mw, ctx);

	json_t* o = json_object();
	setInt(o, "removed_module_id", id);
	setInt(o, "removed_cables", removedCables);
	ToolResult r;
	r.content = okJson(o);
	r.summary = name + " removed";
	r.mutated = true;
	return r;
}


// ---- move_module ---------------------------------------------------------------------

static ToolResult moveModule(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);
	ModuleRef m;
	if (!moduleArg(args, "module_id", &m, &err))
		return errorResult(err);
	int reqHp = 0, reqRow = 0;
	json_t* posJ = json_object_get(args, "position");
	if (!posJ)
		return errorResult("Missing 'position': {\"hp\": int, \"row\": int}.");
	if (!parsePosition(posJ, &reqHp, &reqRow, &err))
		return errorResult(err);

	int w = widthHp(m.mw);
	Occupancy occ(m.mw);
	GridPos target;
	target.hp = reqHp;
	target.row = reqRow;
	std::string note;
	if (!occ.isFree(reqHp, reqRow, w)) {
		target.hp = occ.findFreeHp(reqHp, reqRow, w, false);
		note = string::f("Requested position HP %d, row %d is occupied; moved to HP %d, row %d instead.", reqHp, reqRow, target.hp, target.row);
	}

	math::Vec oldPos = m.mw->box.pos;
	math::Vec newPos = gridToPx(target.hp, target.row);
	bool changed = !oldPos.equals(newPos);
	if (changed) {
		history::ModuleMove* mm = new history::ModuleMove;
		mm->moduleId = m.module->id;
		mm->oldPos = oldPos;
		mm->newPos = newPos;
		ctx.undo->push(mm);
		m.mw->box.pos = newPos;
		APP->scene->rack->updateExpanders();
	}
	else if (note.empty()) {
		note = "Module was already at that position.";
	}

	json_t* o = json_object();
	setInt(o, "module_id", m.module->id);
	setJson(o, "pos", posJson(target.hp, target.row));
	if (!note.empty())
		setStr(o, "note", note);
	ToolResult r;
	r.content = okJson(o);
	r.summary = string::f("%s moved to HP %d, row %d", moduleName(m).c_str(), target.hp, target.row);
	r.mutated = changed;
	return r;
}


// ---- set_param -----------------------------------------------------------------------

static ToolResult setParam(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);
	ModuleRef m;
	if (!moduleArg(args, "module_id", &m, &err))
		return errorResult(err);

	json_t* valueJ = json_object_get(args, "value");
	json_t* dispJ = json_object_get(args, "display_value");
	bool hasValue = valueJ && !json_is_null(valueJ);
	bool hasDisp = dispJ && !json_is_null(dispJ);
	if (!hasValue && !hasDisp)
		return errorResult("Provide one of 'value' (raw knob value) or 'display_value' (value in the displayed unit). Use get_module_info to see ranges and display units.");
	std::string bothNote;
	bool bothZero = false;
	if (hasValue && hasDisp) {
		// Models often fill both fields and put a placeholder 0 into the one they do not mean
		// ({"value":0.72,"display_value":0}). A zero loses against a non-zero number; if both are
		// non-zero the displayed unit is what the model reasons about, so display_value wins.
		double v = json_is_number(valueJ) ? json_number_value(valueJ) : 0.0;
		double d = json_is_number(dispJ) ? json_number_value(dispJ) : 0.0;
		if (d == 0.0 && v != 0.0) {
			hasDisp = false;
			bothNote = "Both 'value' and 'display_value' were given; display_value 0 was treated as a placeholder and the non-zero 'value' was used. If you meant display 0, call set_param again with only display_value. Send only one of them.";
		}
		else if (v == 0.0 && d != 0.0) {
			hasValue = false;
			bothNote = "Both 'value' and 'display_value' were given; value 0 was treated as a placeholder and the non-zero 'display_value' was used. If you meant raw 0, call set_param again with only value. Send only one of them.";
		}
		else if (v == 0.0) {
			hasDisp = false;
			bothZero = true;
			bothNote = "Both 'value' and 'display_value' were 0; 'value' was used. Send only one of them.";
		}
		else {
			hasValue = false;
			bothNote = "Both 'value' and 'display_value' were given; display_value was used and value ignored. Send only one of them.";
		}
	}
	float requested = 0.f;
	if (!argFloat(args, hasValue ? "value" : "display_value", &requested))
		return errorResult(string::f("Invalid '%s': expected a finite number.", hasValue ? "value" : "display_value"));

	int paramId = -1;
	if (!resolveParam(m, json_object_get(args, "param_id"), &paramId, &err))
		return errorResult(err);
	engine::ParamQuantity* pq = paramQuantity(m.module, paramId);
	if (!pq)
		return errorResult("Parameter is not controllable.");

	float minV = pq->getMinValue();
	float maxV = pq->getMaxValue();
	float oldV = m.module->params[paramId].getValue();
	bool clamped = false;
	std::string note;
	if (hasValue) {
		clamped = requested < minV || requested > maxV;
		pq->setImmediateValue(requested);
	}
	else {
		pq->setDisplayValue(requested);
	}
	float newV = pq->getImmediateValue();
	if (!hasValue) {
		// Detect that the display value was outside the reachable range (value pinned at a limit
		// and the displayed number differs from the requested one).
		float shown = pq->getDisplayValue();
		float tol = 1e-3f * std::max(1.f, std::fabs(requested));
		if (std::fabs(shown - requested) > tol) {
			if (newV == minV || newV == maxV)
				clamped = true;
			else if (newV == oldV)
				note = "The display value could not be applied (outside the reachable range or not representable).";
		}
	}

	if (bothZero) {
		// Raw 0 and display 0 only agree for some parameters. Say so loudly when they do not.
		float shown = pq->getDisplayValue();
		if (std::fabs(shown) > 1e-3f)
			note = string::f("Raw value 0 was applied and it displays as %g, not 0. If you meant display 0, call set_param again with only display_value.", shown);
	}

	bool changed = newV != oldV;
	if (changed) {
		history::ParamChange* h = new history::ParamChange;
		h->moduleId = m.module->id;
		h->paramId = paramId;
		h->oldValue = oldV;
		h->newValue = newV;
		ctx.undo->push(h);
	}

	std::string name = pq->getLabel();
	std::string display = displayString(pq);
	json_t* o = json_object();
	setInt(o, "module_id", m.module->id);
	setInt(o, "param_id", paramId);
	setStr(o, "name", name);
	setNum(o, "old_value", oldV);
	setNum(o, "value", newV);
	setStr(o, "display", display);
	setNum(o, "min", minV);
	setNum(o, "max", maxV);
	setBool(o, "clamped", clamped);
	if (!bothNote.empty())
		note += (note.empty() ? "" : " ") + bothNote;
	if (!note.empty())
		setStr(o, "note", note);
	ToolResult r;
	r.content = okJson(o);
	r.summary = name + " \xE2\x86\x92 " + display;
	r.mutated = changed;
	return r;
}


// ---- connect / disconnect ------------------------------------------------------------

static bool isHexColor(const std::string& s) {
	size_t n = s.size();
	size_t i = (n > 0 && s[0] == '#') ? 1 : 0;
	size_t digits = n - i;
	// Only #rrggbb: an alpha pair could produce a fully transparent, invisible cable
	if (digits != 6)
		return false;
	for (; i < n; i++) {
		if (!isxdigit((unsigned char) s[i]))
			return false;
	}
	return true;
}


/** Removes a cable (recorded in undo). The widget is deleted. */
static void removeCableWithHistory(app::CableWidget* cw, ToolContext& ctx) {
	history::CableRemove* h = new history::CableRemove;
	try {
		h->setCable(cw);
	}
	catch (...) {
		delete h;
		throw;
	}
	ctx.undo->push(h);
	APP->scene->rack->removeCable(cw);
	delete cw;
}


static ToolResult connectTool(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);

	// 1. Validate everything before touching the engine
	ModuleRef from, to;
	if (!moduleArg(args, "from_module", &from, &err))
		return errorResult(err);
	if (!moduleArg(args, "to_module", &to, &err))
		return errorResult(err);
	int outId = -1, inId = -1;
	if (!resolvePort(from, false, json_object_get(args, "output_id"), &outId, &err))
		return errorResult(err);
	if (!resolvePort(to, true, json_object_get(args, "input_id"), &inId, &err))
		return errorResult(err);
	bool replace = false;
	if (!optBool(args, "replace", false, &replace, &err))
		return errorResult(err);
	bool hasColor = false;
	std::string colorStr;
	json_t* colorJ = json_object_get(args, "color");
	if (colorJ && !json_is_null(colorJ)) {
		if (!argString(args, "color", &colorStr) || !isHexColor(trim(colorStr)))
			return errorResult("Invalid 'color': expected a hex color like \"#ff8800\".");
		colorStr = trim(colorStr);
		if (colorStr[0] != '#')
			colorStr = "#" + colorStr;
		hasColor = true;
	}

	app::RackWidget* rack = APP->scene->rack;
	app::PortWidget* outPw = from.mw->getOutput(outId);
	app::PortWidget* inPw = to.mw->getInput(inId);
	if (!outPw || !inPw)
		return errorResult("Port has no widget on the panel and cannot be connected.");

	// 2. Duplicate / occupied input
	app::CableWidget* existing = rack->getCable(outPw, inPw);
	if (existing && existing->cable) {
		json_t* o = json_object();
		setInt(o, "cable_id", existing->cable->id);
		setJson(o, "from", portEndJson(from.module, false, outId));
		setJson(o, "to", portEndJson(to.module, true, inId));
		setJson(o, "replaced_cable_ids", json_array());
		setStr(o, "note", "already connected");
		ToolResult r;
		r.content = okJson(o);
		r.summary = endLabel(from.module, false, outId) + " \xE2\x86\x92 " + endLabel(to.module, true, inId) + " (already connected)";
		r.mutated = false;
		return r;
	}

	std::vector<app::CableWidget*> occupying = cablesOnInput(inPw);
	if (!occupying.empty() && !replace) {
		std::vector<int64_t> ids;
		std::string srcs;
		for (app::CableWidget* cw : occupying) {
			if (!cw->cable)
				continue;
			ids.push_back(cw->cable->id);
			srcs += (srcs.empty() ? "" : "; ") + endLabel(cw->cable->outputModule, false, cw->cable->outputId);
		}
		return errorResult(string::f("Input '%s' of %s is already connected (cable id %s, from %s). Pass replace=true to replace it, pick another input, or call disconnect first.", portName(to.module, true, inId).c_str(), moduleDescription(to).c_str(), joinIds(ids).c_str(), srcs.c_str()));
	}

	// 3. Remove replaced cables
	std::vector<int64_t> replacedIds;
	for (app::CableWidget* cw : occupying) {
		if (cw->cable)
			replacedIds.push_back(cw->cable->id);
		removeCableWithHistory(cw, ctx);
	}

	// 4. Create the cable
	engine::Cable* cable = new engine::Cable;
	cable->inputModule = to.module;
	cable->inputId = inId;
	cable->outputModule = from.module;
	cable->outputId = outId;
	try {
		APP->engine->addCable(cable);
	}
	catch (...) {
		// A plugin onPortChange callback threw; the engine may already hold the cable.
		try {
			if (APP->engine->hasCable(cable))
				APP->engine->removeCable(cable);
		}
		catch (...) {
		}
		if (!APP->engine->hasCable(cable))
			delete cable;
		throw;
	}

	app::CableWidget* cw = new app::CableWidget;
	try {
		cw->setCable(cable);
	}
	catch (...) {
		// The widget did not adopt the cable
		APP->engine->removeCable(cable);
		delete cable;
		delete cw;
		throw;
	}
	cw->color = hasColor ? color::fromHexString(colorStr) : rack->getNextCableColor();
	rack->addCable(cw);

	history::CableAdd* ha = new history::CableAdd;
	try {
		ha->setCable(cw);
	}
	catch (...) {
		delete ha;
		throw;
	}
	ctx.undo->push(ha);

	json_t* o = json_object();
	setInt(o, "cable_id", cable->id);
	setJson(o, "from", portEndJson(from.module, false, outId));
	setJson(o, "to", portEndJson(to.module, true, inId));
	json_t* repl = json_array();
	for (int64_t id : replacedIds)
		appendJson(repl, jInt(id));
	setJson(o, "replaced_cable_ids", repl);
	ToolResult r;
	r.content = okJson(o);
	r.summary = endLabel(from.module, false, outId) + " \xE2\x86\x92 " + endLabel(to.module, true, inId);
	r.mutated = true;
	return r;
}


static ToolResult disconnectTool(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);

	app::RackWidget* rack = APP->scene->rack;
	std::vector<app::CableWidget*> targets;
	if (json_object_get(args, "cable_id") && !json_is_null(json_object_get(args, "cable_id"))) {
		int64_t cableId;
		if (!argInt64(args, "cable_id", &cableId))
			return errorResult("Invalid 'cable_id': expected an integer cable id (see get_patch).");
		app::CableWidget* cw = rack->getCable(cableId);
		if (!cw || !cw->cable || !cw->isComplete())
			return errorResult(string::f("Cable id %lld not found. Call get_patch to see current cable ids.", (long long) cableId));
		targets.push_back(cw);
	}
	else if (json_object_get(args, "to_module")) {
		ModuleRef to;
		if (!moduleArg(args, "to_module", &to, &err))
			return errorResult(err);
		int inId = -1;
		if (!resolvePort(to, true, json_object_get(args, "input_id"), &inId, &err))
			return errorResult(err);
		for (app::CableWidget* cw : cablesOnInput(to.mw->getInput(inId))) {
			if (cw->cable)
				targets.push_back(cw);
		}
		if (targets.empty())
			return errorResult(string::f("No cable is connected to input '%s' of %s.", portName(to.module, true, inId).c_str(), moduleDescription(to).c_str()));
	}
	else {
		return errorResult("Provide either 'cable_id' or both 'to_module' and 'input_id'.");
	}

	std::string label = targets.size() == 1 ? cableLabel(targets[0]) : string::f("%d cables", (int) targets.size());
	std::vector<int64_t> removed;
	for (app::CableWidget* cw : targets) {
		removed.push_back(cw->cable->id);
		removeCableWithHistory(cw, ctx);
	}

	json_t* o = json_object();
	json_t* ids = json_array();
	for (int64_t id : removed)
		appendJson(ids, jInt(id));
	setJson(o, "removed_cable_ids", ids);
	ToolResult r;
	r.content = okJson(o);
	r.summary = "Disconnected " + label;
	r.mutated = true;
	return r;
}


// ---- save_patch ----------------------------------------------------------------------

/** Canonical form of the deepest existing ancestor of `path` plus the not yet existing rest.
Returns "" if nothing could be resolved. */
static std::string canonicalExisting(const std::string& path) {
	std::string existing = path;
	std::string rest;
	while (!existing.empty() && !system::exists(existing)) {
		std::string parent = system::getDirectory(existing);
		if (parent == existing)
			return "";
		rest = "/" + system::getFilename(existing) + rest;
		existing = parent;
	}
	if (existing.empty())
		return "";
	std::string canon = system::getCanonical(existing);
	if (canon.empty())
		return "";
	return canon + rest;
}


/** Resolves the target file. Relative paths live in <user>/patches. */
static bool resolveSavePath(json_t* args, std::string* path, std::string* err) {
	std::string p;
	json_t* pj = json_object_get(args, "path");
	if (pj && !json_is_null(pj)) {
		if (!argString(args, "path", &p)) {
			*err = "Invalid 'path': expected a string.";
			return false;
		}
	}
	p = trim(p);
	if (p.empty()) {
		if (APP->patch->path.empty()) {
			*err = "The patch has no file yet. Call save_patch with a 'path' (a file name like \"my-patch.vcv\"; it is stored in the Rack user patches folder).";
			return false;
		}
		*path = APP->patch->path;
		return true;
	}

	std::replace(p.begin(), p.end(), '\\', '/');
	for (char c : p) {
		if ((unsigned char) c < 0x20) {
			*err = "Invalid 'path': contains control characters.";
			return false;
		}
	}
	if (p[0] == '/' || p[0] == '~' || (p.size() > 1 && p[1] == ':')) {
		*err = "Invalid 'path': absolute paths are not allowed. Give a file name relative to the Rack user patches folder, e.g. \"my-patch.vcv\" or \"acid/lead.vcv\".";
		return false;
	}
	for (const std::string& part : string::split(p, "/")) {
		if (part == "..") {
			*err = "Invalid 'path': '..' is not allowed.";
			return false;
		}
	}
	// Always write a .vcv file. Other extensions get ".vcv" appended.
	if (string::lowercase(system::getExtension(p)) != ".vcv")
		p += ".vcv";
	std::string root = asset::user("patches");
	std::string full = system::join(root, p);

	// Symlinks inside the patches folder must not lead outside of it. Canonicalize the deepest
	// existing part of the path (the file itself if it exists, else its closest existing parent)
	// and require it to stay under the canonical patches folder.
	std::string rootCanon = canonicalExisting(root);
	std::string fullCanon = canonicalExisting(full);
	if (rootCanon.empty() || fullCanon.size() <= rootCanon.size() || fullCanon.compare(0, rootCanon.size(), rootCanon) != 0 || fullCanon[rootCanon.size()] != '/') {
		*err = "Invalid 'path': the target resolves outside the Rack user patches folder.";
		return false;
	}
	*path = full;
	return true;
}


static std::string savePatchConfirmation(json_t* args) {
	std::string path, err;
	if (!resolveSavePath(args, &path, &err))
		return "";
	if (!system::exists(path))
		return "";
	if (!APP->patch->path.empty() && system::getAbsolute(path) == system::getAbsolute(APP->patch->path))
		return "";
	return "Overwrite existing file " + path + "?";
}


static ToolResult savePatch(json_t* args, ToolContext& ctx) {
	std::string path, err;
	if (!resolveSavePath(args, &path, &err))
		return errorResult(err);

	// Like Rack's saveDialog(): mark saved BEFORE save(), otherwise toJson() writes "unsaved": true
	// into the patch. Restore the previous saved index if saving fails.
	int prevSavedIndex = APP->history->savedIndex;
	APP->history->setSaved();
	try {
		std::string dir = system::getDirectory(path);
		if (!dir.empty())
			system::createDirectories(dir);
		APP->patch->save(path);
	}
	catch (const std::exception& e) {
		APP->history->savedIndex = prevSavedIndex;
		return errorResult(string::f("Saving failed: %s", e.what()));
	}
	APP->patch->path = path;
	APP->patch->pushRecentPath(path);
	ctx.savedDuringRun = true;
	ctx.mutationsSinceSave = 0;

	json_t* o = json_object();
	setStr(o, "path", path);
	ToolResult r;
	r.content = okJson(o);
	r.summary = "Saved " + system::getFilename(path);
	r.mutated = false;
	return r;
}


// ---- clear_patch ---------------------------------------------------------------------

static std::string clearPatchConfirmation(json_t* args) {
	size_t modules = APP->scene->rack->getModules().size();
	if (modules == 0)
		return "";
	size_t cables = APP->scene->rack->getCompleteCables().size();
	return string::f("Remove all %d modules and %d cables from the patch?", (int) modules, (int) cables);
}


static ToolResult clearPatch(json_t* args, ToolContext& ctx) {
	std::string err;
	if (!requireUndo(ctx, &err))
		return errorResult(err);

	std::vector<app::ModuleWidget*> mws = APP->scene->rack->getModules();
	int removedModules = 0;
	int removedCables = 0;
	for (app::ModuleWidget* mw : mws) {
		if (!mw->module)
			continue;
		removedCables += removeModuleWithHistory(mw, ctx);
		removedModules++;
	}

	json_t* o = json_object();
	setInt(o, "removed_modules", removedModules);
	setInt(o, "removed_cables", removedCables);
	ToolResult r;
	r.content = okJson(o);
	r.summary = string::f("Cleared patch (%d modules)", removedModules);
	r.mutated = removedModules > 0;
	return r;
}


// ---- Registration --------------------------------------------------------------------

void registerPatchTools(ToolRegistry& r) {
	{
		Tool t;
		t.name = "get_patch";
		t.description = "Read the current patch: modules (id, plugin, model, position in HP/row, width, bypass state, parameters that differ from their defaults), cables (id, from output port to input port), and the selected module ids. Call this before modifying an existing patch, and to find module and cable ids.";
		t.parametersSchema = R"({"type":"object","properties":{"include_all_params":{"type":"boolean","description":"List every parameter, not only those changed from default. Default false."},"module_ids":{"type":"array","items":{"type":"integer"},"description":"Only list these modules (and cables touching them)."}}})";
		t.run = getPatch;
		r.add(t);
	}
	{
		Tool t;
		t.name = "add_module";
		t.description = "Add a module to the patch. Use the plugin and model slugs exactly as returned by search_modules. Without a position the module is placed in the first free slot to the right of the previously added module (modules never overlap). Positions are in grid units: hp (1 HP = 15 px, horizontal) and row (0 = first row).";
		t.parametersSchema = R"({"type":"object","properties":{"plugin":{"type":"string","description":"Plugin slug, e.g. \"Fundamental\"."},"model":{"type":"string","description":"Model slug, e.g. \"VCO\"."},"position":{"type":"object","properties":{"hp":{"type":"integer"},"row":{"type":"integer"}},"required":["hp"],"description":"Optional explicit position. If occupied, the nearest free slot in that row is used."},"near_module_id":{"type":"integer","description":"Optional: id of a module that already exists in the patch; place in the first free slot to its right. Omit it otherwise (never send 0 or -1 as a placeholder)."}},"required":["plugin","model"]})";
		t.run = addModule;
		r.add(t);
	}
	{
		Tool t;
		t.name = "remove_module";
		t.description = "Remove a module and all cables connected to it (undoable). The user is asked to confirm.";
		t.parametersSchema = R"({"type":"object","properties":{"module_id":{"type":"integer"}},"required":["module_id"]})";
		t.confirmation = removeModuleConfirmation;
		t.run = removeModule;
		r.add(t);
	}
	{
		Tool t;
		t.name = "move_module";
		t.description = "Move a module to a grid position (hp, row). If the target slot is occupied the nearest free slot in that row is used.";
		t.parametersSchema = R"({"type":"object","properties":{"module_id":{"type":"integer"},"position":{"type":"object","properties":{"hp":{"type":"integer"},"row":{"type":"integer"}},"required":["hp"]}},"required":["module_id","position"]})";
		t.run = moveModule;
		r.add(t);
	}
	{
		Tool t;
		t.name = "set_param";
		t.description = "Set a module parameter (knob, switch, button). param_id is the integer id or the parameter name (case-insensitive). Give exactly one of: value (the raw knob value within the parameter's min..max) or display_value (the number as shown on screen, e.g. 440 for 440 Hz). If you send both, a 0 in one of them is treated as a placeholder for the other, so send only the one you mean. Values are clamped to the range; the result reports the final value and whether it was clamped. Use get_module_info for ranges, units and switch options.";
		t.parametersSchema = R"({"type":"object","properties":{"module_id":{"type":"integer"},"param_id":{"type":["integer","string"],"description":"Parameter index or name."},"value":{"type":"number","description":"Raw knob value. Send either this or display_value, never both."},"display_value":{"type":"number","description":"Value in displayed units (e.g. Hz, ms, %). Send either this or value, never both."}},"required":["module_id","param_id"]})";
		t.run = setParam;
		r.add(t);
	}
	{
		Tool t;
		t.name = "connect";
		t.description = "Connect an output port to an input port with a cable. Ports are integer ids or port names (case-insensitive). An input accepts one cable: if it is already connected the call fails unless replace=true. Connecting the same ports twice is a no-op.";
		t.parametersSchema = R"({"type":"object","properties":{"from_module":{"type":"integer"},"output_id":{"type":["integer","string"],"description":"Output port index or name."},"to_module":{"type":"integer"},"input_id":{"type":["integer","string"],"description":"Input port index or name."},"color":{"type":"string","description":"Optional cable color like #ff8800."},"replace":{"type":"boolean","description":"Replace an existing cable on the input. Default false."}},"required":["from_module","output_id","to_module","input_id"]})";
		t.run = connectTool;
		r.add(t);
	}
	{
		Tool t;
		t.name = "disconnect";
		t.description = "Remove a cable, either by cable_id or by the input it is plugged into (to_module + input_id). Undoable.";
		t.parametersSchema = R"({"type":"object","properties":{"cable_id":{"type":"integer"},"to_module":{"type":"integer"},"input_id":{"type":["integer","string"],"description":"Input port index or name."}}})";
		t.run = disconnectTool;
		r.add(t);
	}
	{
		Tool t;
		t.name = "save_patch";
		t.description = "Save the patch. Without a path the current patch file is overwritten (an untitled patch needs a path). A relative path is stored in the Rack user patches folder; \".vcv\" is appended unless the name already ends in .vcv. Overwriting a different existing file asks the user to confirm.";
		t.parametersSchema = R"({"type":"object","properties":{"path":{"type":"string","description":"File name relative to the user patches folder, e.g. \"my-acid.vcv\"."}}})";
		t.confirmation = savePatchConfirmation;
		t.run = savePatch;
		r.add(t);
	}
	{
		Tool t;
		t.name = "clear_patch";
		t.description = "Remove all modules and cables from the patch (undoable). The user is asked to confirm.";
		t.parametersSchema = R"({"type":"object","properties":{}})";
		t.confirmation = clearPatchConfirmation;
		t.run = clearPatch;
		r.add(t);
	}
}


} // namespace assistant
} // namespace rack

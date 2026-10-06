#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <FuzzySearchDatabase.hpp>

#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <tag.hpp>


namespace rack {
namespace assistant {


using namespace helpers;


// ---- Visibility and fuzzy index ------------------------------------------------------

/** Same filter as Browser::isModelVisible (without brand/tag/favorite filters). */
static bool isModelVisible(plugin::Model* model) {
	if (model->hidden)
		return false;
	settings::ModuleInfo* mi = settings::getModuleInfo(model->plugin->slug, model->slug);
	if (mi && !mi->enabled)
		return false;
	if (!settings::isModuleWhitelisted(model->plugin->slug, model->slug))
		return false;
	return true;
}


static size_t countModels() {
	size_t n = 0;
	for (plugin::Plugin* p : plugin::plugins)
		n += p->models.size();
	return n;
}


struct SearchIndex {
	fuzzysearch::Database<plugin::Model*> db;
	size_t modelCount = (size_t) -1;
};


/** Builds (or rebuilds if the model list changed) the fuzzy database, same fields and weights as Browser.cpp. */
static SearchIndex& getIndex() {
	static SearchIndex index;
	size_t n = countModels();
	if (index.modelCount == n)
		return index;
	index.modelCount = n;
	index.db = fuzzysearch::Database<plugin::Model*>();
	index.db.setWeights({0.9f, 0.75f, 1.0f, 0.8f, 0.9f});
	index.db.setThreshold(0.5f);
	for (plugin::Plugin* p : plugin::plugins) {
		for (plugin::Model* model : p->models) {
			std::string tagStr;
			for (int tagId : model->tagIds) {
				if (tagId < 0 || tagId >= (int) tag::tagAliases.size())
					continue;
				if (settings::language != "en") {
					tagStr += string::translate("tag." + tag::getTag(tagId), settings::language);
					tagStr += " ";
				}
				for (const std::string& alias : tag::tagAliases[tagId]) {
					tagStr += alias;
					tagStr += " ";
				}
			}
			std::vector<std::string> fields = {
				model->plugin->brand,
				model->plugin->name,
				model->name,
				model->description,
				tagStr,
			};
			index.db.addEntry(model, fields);
		}
	}
	return index;
}


static std::vector<std::string> tagNames(plugin::Model* model) {
	std::vector<std::string> names;
	for (int tagId : model->tagIds) {
		if (tagId >= 0 && tagId < (int) tag::tagAliases.size())
			names.push_back(tag::getTag(tagId));
	}
	return names;
}


static json_t* tagsJson(plugin::Model* model) {
	json_t* arr = json_array();
	for (const std::string& n : tagNames(model))
		appendJson(arr, jStr(n));
	return arr;
}


// ---- search_modules ------------------------------------------------------------------

static bool contains(const std::string& haystackLower, const std::string& needleLower) {
	return haystackLower.find(needleLower) != std::string::npos;
}


static ToolResult searchModules(json_t* args, ToolContext& ctx) {
	std::string query;
	json_t* qj = json_object_get(args, "query");
	if (qj && !json_is_null(qj)) {
		if (!argString(args, "query", &query))
			return errorResult("Invalid 'query': expected a string.");
	}
	query = trim(query);

	std::string tagName;
	int tagId = -1;
	json_t* tj = json_object_get(args, "tag");
	if (tj && !json_is_null(tj)) {
		if (!argString(args, "tag", &tagName))
			return errorResult("Invalid 'tag': expected a string.");
		tagName = trim(tagName);
	}
	if (!tagName.empty()) {
		tagId = tag::findId(tagName);
		if (tagId < 0) {
			std::vector<std::string> names;
			for (const std::vector<std::string>& aliases : tag::tagAliases)
				names.push_back(aliases[0]);
			std::sort(names.begin(), names.end());
			return errorResult(string::f("Unknown tag '%s'. Valid tags: %s.", tagName.c_str(), string::join(names, ", ").c_str()));
		}
	}

	if (query.empty() && tagId < 0)
		return errorResult("Provide a 'query' (search text) and/or a 'tag'.");

	int64_t limit = 12;
	json_t* lj = json_object_get(args, "limit");
	if (lj && !json_is_null(lj)) {
		if (!argInt64(args, "limit", &limit))
			return errorResult("Invalid 'limit': expected an integer between 1 and 40.");
	}
	limit = std::max<int64_t>(1, std::min<int64_t>(40, limit));

	std::vector<plugin::Model*> matches;
	std::set<plugin::Model*> seen;
	auto accept = [&](plugin::Model* m) {
		if (!m || !m->plugin || seen.count(m))
			return;
		if (!isModelVisible(m))
			return;
		if (tagId >= 0 && std::find(m->tagIds.begin(), m->tagIds.end(), tagId) == m->tagIds.end())
			return;
		seen.insert(m);
		matches.push_back(m);
	};

	if (query.empty()) {
		for (plugin::Plugin* p : plugin::plugins) {
			for (plugin::Model* m : p->models)
				accept(m);
		}
		std::sort(matches.begin(), matches.end(), [](plugin::Model* a, plugin::Model* b) {
			std::string ka = lowercase(a->plugin->getBrand() + " " + a->name);
			std::string kb = lowercase(b->plugin->getBrand() + " " + b->name);
			return ka < kb;
		});
	}
	else {
		auto results = getIndex().db.search(query);
		for (const auto& result : results)
			accept(result.key);
		// Substring fallback on slug and name
		std::string ql = lowercase(query);
		for (plugin::Plugin* p : plugin::plugins) {
			for (plugin::Model* m : p->models) {
				if (contains(lowercase(m->slug), ql) || contains(lowercase(m->name), ql))
					accept(m);
			}
		}
	}

	json_t* o = json_object();
	setInt(o, "total_matches", (int64_t) matches.size());
	json_t* arr = json_array();
	int returned = 0;
	for (plugin::Model* m : matches) {
		if (returned >= limit)
			break;
		json_t* item = json_object();
		setStr(item, "plugin", m->plugin->slug);
		setStr(item, "model", m->slug);
		setStr(item, "name", m->name);
		setStr(item, "brand", m->plugin->getBrand());
		setJson(item, "tags", tagsJson(m));
		setStr(item, "description", m->description);
		appendJson(arr, item);
		returned++;
	}
	setJson(o, "results", arr);
	if ((int) matches.size() > returned)
		setStr(o, "note", string::f("Showing %d of %d matches; refine the query or use a tag to narrow down.", returned, (int) matches.size()));

	ToolResult r;
	r.readOnly = true;
	r.content = okJson(o);
	std::string what = query.empty() ? ("tag " + tagName) : ("\"" + query + "\"");
	r.summary = string::f("Searched %s (%d)", what.c_str(), (int) matches.size());
	return r;
}


// ---- get_module_info -----------------------------------------------------------------

/** Deletes a temporary, never-added module and widget even when an exception is thrown. */
struct TempModel {
	engine::Module* module = NULL;
	app::ModuleWidget* widget = NULL;
	~TempModel() {
		delete widget;
		delete module;
	}
};


static void setDisplayForValue(engine::Module* module, engine::ParamQuantity* pq, int id, float v, json_t* obj, const char* key) {
	if (!std::isfinite(v))
		return;
	module->params[id].setValue(v);
	setStr(obj, key, displayString(pq));
}


/** Computes the model-level info (no live values) as JSON text. */
static std::string buildModelInfo(plugin::Model* model) {
	TempModel tmp;
	tmp.module = model->createModule();
	if (!tmp.module)
		throw Exception("The plugin failed to create the module.");
	engine::Module* module = tmp.module;

	json_t* root = json_object();
	JsonPtr guard(root);
	setStr(root, "plugin", model->plugin->slug);
	setStr(root, "model", model->slug);
	setStr(root, "name", model->name);
	setStr(root, "brand", model->plugin->getBrand());
	setStr(root, "description", model->description);
	setJson(root, "tags", tagsJson(model));

	// Width from a widget that is not attached to any module
	int width = 0;
	try {
		tmp.widget = model->createModuleWidget(NULL);
		if (tmp.widget)
			width = widthHp(tmp.widget);
	}
	catch (const std::exception&) {
		width = 0;
	}
	if (width > 0)
		setInt(root, "width_hp", width);

	json_t* paramsJ = json_array();
	for (size_t i = 0; i < module->params.size(); i++) {
		engine::ParamQuantity* pq = paramQuantity(module, (int) i);
		if (!pq)
			continue;
		json_t* p = json_object();
		setInt(p, "id", (int64_t) i);
		setStr(p, "name", pq->getLabel());
		float minV = pq->getMinValue();
		float maxV = pq->getMaxValue();
		float defV = pq->getDefaultValue();
		setNum(p, "min", minV);
		setNum(p, "max", maxV);
		setNum(p, "default", defV);
		setStr(p, "unit", pq->getUnit());
		setBool(p, "snap", pq->snapEnabled);
		std::string desc = pq->getDescription();
		if (!desc.empty())
			setStr(p, "description", desc);
		engine::SwitchQuantity* sq = dynamic_cast<engine::SwitchQuantity*>(pq);
		if (sq) {
			json_t* opts = json_array();
			for (const std::string& l : sq->labels)
				appendJson(opts, jStr(l));
			setJson(p, "options", opts);
		}
		// Display strings at min, max and default (value is written directly on the temporary module)
		setDisplayForValue(module, pq, (int) i, minV, p, "min_display");
		setDisplayForValue(module, pq, (int) i, maxV, p, "max_display");
		setDisplayForValue(module, pq, (int) i, defV, p, "default_display");
		module->params[i].setValue(defV);
		appendJson(paramsJ, p);
	}
	setJson(root, "params", paramsJ);

	for (int pass = 0; pass < 2; pass++) {
		bool input = pass == 0;
		size_t n = input ? module->inputs.size() : module->outputs.size();
		const std::vector<engine::PortInfo*>& infos = input ? module->inputInfos : module->outputInfos;
		json_t* arr = json_array();
		for (size_t i = 0; i < n; i++) {
			json_t* p = json_object();
			setInt(p, "id", (int64_t) i);
			setStr(p, "name", portName(module, input, (int) i));
			if (i < infos.size() && infos[i]) {
				std::string desc = infos[i]->getDescription();
				if (!desc.empty())
					setStr(p, "description", desc);
			}
			appendJson(arr, p);
		}
		setJson(root, input ? "inputs" : "outputs", arr);
	}

	return dumpCompact(root);
}


static std::map<plugin::Model*, std::string>& modelInfoCache() {
	static std::map<plugin::Model*, std::string> cache;
	return cache;
}


static const std::string& getModelInfo(plugin::Model* model) {
	std::map<plugin::Model*, std::string>& cache = modelInfoCache();
	auto it = cache.find(model);
	if (it != cache.end())
		return it->second;
	std::string text = buildModelInfo(model);
	return cache.insert(std::make_pair(model, text)).first->second;
}


static ToolResult getModuleInfo(json_t* args, ToolContext& ctx) {
	std::string err;
	ModuleRef inst;
	plugin::Model* model = NULL;
	bool isInstance = false;

	json_t* idj = json_object_get(args, "module_id");
	if (idj && !json_is_null(idj)) {
		if (!moduleArg(args, "module_id", &inst, &err))
			return errorResult(err);
		model = inst.module->model;
		isInstance = true;
		if (!model)
			return errorResult("This module has no model information.");
	}
	else {
		std::string pluginSlug, modelSlug;
		if (!argString(args, "plugin", &pluginSlug) || !argString(args, "model", &modelSlug) || trim(pluginSlug).empty() || trim(modelSlug).empty())
			return errorResult("Provide either 'module_id' (a module in the patch) or both 'plugin' and 'model' (slugs from search_modules).");
		pluginSlug = trim(pluginSlug);
		modelSlug = trim(modelSlug);
		model = plugin::getModel(pluginSlug, modelSlug);
		if (!model)
			return errorResult(string::f("Model '%s/%s' not found. Use search_modules to find the exact plugin and model slugs.", pluginSlug.c_str(), modelSlug.c_str()));
	}

	JsonPtr info;
	{
		json_error_t error;
		info.reset(json_loads(getModelInfo(model).c_str(), 0, &error));
	}
	if (!info || !json_is_object(info.get()))
		return errorResult("Internal error: could not build module info.");

	std::string name = model->name;
	if (isInstance) {
		engine::Module* module = inst.module;
		setInt(info.get(), "module_id", module->id);
		GridPos gp = gridPosOf(inst.mw);
		setJson(info.get(), "pos", posJson(gp.hp, gp.row));
		setInt(info.get(), "width_hp", widthHp(inst.mw));
		setBool(info.get(), "bypassed", module->isBypassed());

		json_t* paramsJ = json_object_get(info.get(), "params");
		size_t i;
		json_t* p;
		json_array_foreach(paramsJ, i, p) {
			int64_t id = -1;
			if (!argInt64(p, "id", &id))
				continue;
			engine::ParamQuantity* pq = paramQuantity(module, (int) id);
			if (!pq)
				continue;
			setNum(p, "value", module->params[id].getValue());
			setStr(p, "display", displayString(pq));
		}

		app::RackWidget* rack = APP->scene->rack;
		for (int pass = 0; pass < 2; pass++) {
			bool input = pass == 0;
			json_t* arr = json_object_get(info.get(), input ? "inputs" : "outputs");
			json_array_foreach(arr, i, p) {
				int64_t id = -1;
				if (!argInt64(p, "id", &id))
					continue;
				app::PortWidget* pw = input ? inst.mw->getInput((int) id) : inst.mw->getOutput((int) id);
				bool connected = pw && !rack->getCompleteCablesOnPort(pw).empty();
				setBool(p, "connected", connected);
			}
		}
	}

	ToolResult r;
	r.readOnly = true;
	r.content = okJson(info.release());
	r.summary = "Inspected " + name;
	return r;
}


// ---- Registration --------------------------------------------------------------------

void registerCatalogTools(ToolRegistry& r) {
	{
		Tool t;
		t.name = "search_modules";
		t.description = "Search the installed module library (fuzzy search over brand, plugin, module name, description and tags). Returns plugin and model slugs to use with add_module and get_module_info. Optionally filter by a tag such as \"Filter\", \"Oscillator\", \"VCA\", \"Envelope generator\", \"Sequencer\". An empty query with a tag lists modules with that tag.";
		t.parametersSchema = R"({"type":"object","properties":{"query":{"type":"string","description":"Search text, e.g. \"VCO\" or \"reverb\"."},"tag":{"type":"string","description":"Optional tag name filter, e.g. \"Filter\"."},"limit":{"type":"integer","minimum":1,"maximum":40,"description":"Maximum number of results. Default 12."}},"required":["query"]})";
		t.run = searchModules;
		r.add(t);
	}
	{
		Tool t;
		t.name = "get_module_info";
		t.description = "Describe a module: its parameters (id, name, range, default, unit, display text at min/max/default, switch options), input ports and output ports (id, name), and width. Pass module_id for a module in the patch (adds current values and which ports are connected), or plugin + model for any installed module.";
		t.parametersSchema = R"({"type":"object","properties":{"module_id":{"type":"integer","description":"A module in the current patch."},"plugin":{"type":"string"},"model":{"type":"string"}}})";
		t.run = getModuleInfo;
		r.add(t);
	}
}


} // namespace assistant
} // namespace rack

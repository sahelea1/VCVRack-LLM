#include "ToolHelpers.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <string.hpp>


namespace rack {
namespace assistant {
namespace helpers {


// ---- JSON ----------------------------------------------------------------------------

/** Returns the length of a valid UTF-8 sequence starting at s[i], or 0 if invalid. */
static size_t utf8SeqLen(const std::string& s, size_t i) {
	unsigned char c = (unsigned char) s[i];
	size_t n = s.size() - i;
	if (c < 0x80)
		return 1;
	auto cont = [&](size_t k) -> bool {
		return k < n && ((unsigned char) s[i + k] & 0xC0) == 0x80;
	};
	if (c >= 0xC2 && c <= 0xDF) {
		return cont(1) ? 2 : 0;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		if (!cont(1) || !cont(2))
			return 0;
		unsigned char c1 = (unsigned char) s[i + 1];
		if (c == 0xE0 && c1 < 0xA0)
			return 0; // overlong
		if (c == 0xED && c1 >= 0xA0)
			return 0; // surrogate
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		if (!cont(1) || !cont(2) || !cont(3))
			return 0;
		unsigned char c1 = (unsigned char) s[i + 1];
		if (c == 0xF0 && c1 < 0x90)
			return 0; // overlong
		if (c == 0xF4 && c1 >= 0x90)
			return 0; // > U+10FFFF
		return 4;
	}
	return 0;
}


std::string sanitizeUtf8(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		if (s[i] == '\0') {
			// jansson strings are NUL-terminated
			out += '?';
			i++;
			continue;
		}
		size_t len = utf8SeqLen(s, i);
		if (len == 0) {
			out += '?';
			i++;
		}
		else {
			out.append(s, i, len);
			i += len;
		}
	}
	return out;
}


json_t* jStr(const std::string& s) {
	json_t* j = json_string(sanitizeUtf8(s).c_str());
	if (!j)
		j = json_string("");
	return j;
}


json_t* jNum(double v) {
	if (!std::isfinite(v))
		return json_null();
	char buf[64];
	snprintf(buf, sizeof(buf), "%.6g", v);
	return json_real(strtod(buf, NULL));
}


json_t* jInt(int64_t v) {
	return json_integer((json_int_t) v);
}


json_t* jBool(bool b) {
	return b ? json_true() : json_false();
}


void setJson(json_t* obj, const char* key, json_t* v) {
	json_object_set_new(obj, key, v);
}

void appendJson(json_t* arr, json_t* v) {
	json_array_append_new(arr, v);
}

void setStr(json_t* obj, const char* key, const std::string& v) {
	setJson(obj, key, jStr(v));
}

void setNum(json_t* obj, const char* key, double v) {
	setJson(obj, key, jNum(v));
}

void setInt(json_t* obj, const char* key, int64_t v) {
	setJson(obj, key, jInt(v));
}

void setBool(json_t* obj, const char* key, bool v) {
	setJson(obj, key, jBool(v));
}


json_t* posJson(int hp, int row) {
	json_t* o = json_object();
	setInt(o, "hp", hp);
	setInt(o, "row", row);
	return o;
}


std::string lowercase(const std::string& s) {
	std::string r = s;
	for (char& c : r) {
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
	}
	return r;
}


std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char) s[a]))
		a++;
	while (b > a && isspace((unsigned char) s[b - 1]))
		b--;
	return s.substr(a, b - a);
}


std::string dumpCompact(json_t* j) {
	if (!j)
		return "";
	char* s = json_dumps(j, JSON_COMPACT | JSON_REAL_PRECISION(9));
	if (!s)
		return "";
	std::string r = s;
	free(s);
	return r;
}


// ---- Module / port / param lookup ----------------------------------------------------

bool findModule(int64_t id, ModuleRef* out, std::string* err) {
	engine::Module* module = NULL;
	app::ModuleWidget* mw = NULL;
	if (id >= 0 && APP->engine && APP->scene && APP->scene->rack) {
		module = APP->engine->getModule(id);
		mw = APP->scene->rack->getModule(id);
	}
	if (!module || !mw) {
		if (err)
			*err = string::f("Module id %lld not found. Call get_patch to see current module ids.", (long long) id);
		return false;
	}
	out->module = module;
	out->mw = mw;
	return true;
}


bool moduleArg(json_t* args, const char* key, ModuleRef* out, std::string* err) {
	int64_t id = -1;
	if (!argInt64(args, key, &id)) {
		if (err)
			*err = string::f("Missing or invalid '%s': expected a module id (integer). Call get_patch to see module ids.", key);
		return false;
	}
	return findModule(id, out, err);
}


std::string moduleName(const ModuleRef& m) {
	if (m.module && m.module->model) {
		if (!m.module->model->name.empty())
			return m.module->model->name;
		return m.module->model->slug;
	}
	return "module";
}


std::string moduleDescription(const ModuleRef& m) {
	std::string plugin;
	if (m.module && m.module->model && m.module->model->plugin)
		plugin = m.module->model->plugin->slug;
	return string::f("'%s' (%s %s, id %lld)", moduleName(m).c_str(), plugin.c_str(), moduleName(m).c_str(), (long long) (m.module ? m.module->id : -1));
}


std::string portName(engine::Module* module, bool input, int idx) {
	const std::vector<engine::PortInfo*>& infos = input ? module->inputInfos : module->outputInfos;
	if (idx >= 0 && idx < (int) infos.size() && infos[idx]) {
		std::string n = infos[idx]->getName();
		if (!n.empty())
			return n;
	}
	return string::f("#%d", idx + 1);
}


engine::ParamQuantity* paramQuantity(engine::Module* module, int idx) {
	if (!module)
		return NULL;
	if (idx < 0 || idx >= (int) module->params.size())
		return NULL;
	if (idx >= (int) module->paramQuantities.size())
		return NULL;
	return module->paramQuantities[idx];
}


std::string paramName(engine::Module* module, int idx) {
	engine::ParamQuantity* pq = paramQuantity(module, idx);
	if (!pq)
		return string::f("#%d", idx + 1);
	return pq->getLabel();
}


static const size_t LIST_LIMIT = 40;


/** Parses a numeric id given as JSON integer / integral real. */
static bool jsonToIndex(json_t* j, int64_t* out) {
	if (json_is_integer(j)) {
		*out = (int64_t) json_integer_value(j);
		return true;
	}
	if (json_is_real(j)) {
		double v = json_real_value(j);
		if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 9e15) {
			*out = (int64_t) v;
			return true;
		}
	}
	return false;
}


static bool isDecimalString(const std::string& s) {
	if (s.empty())
		return false;
	size_t i = 0;
	if (s[0] == '-' || s[0] == '+')
		i = 1;
	if (i >= s.size())
		return false;
	for (; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
	}
	return true;
}


/** Shared resolution logic for ports and params.
names[i] is the name of id i, or "" if id i is not addressable. */
static bool resolveId(const std::string& kind, const std::string& owner, const std::vector<std::string>& names, json_t* idJ, int* out, std::string* err) {
	auto listing = [&]() -> std::string {
		std::string s;
		size_t shown = 0, total = 0;
		for (size_t i = 0; i < names.size(); i++) {
			if (names[i].empty())
				continue;
			total++;
			if (shown >= LIST_LIMIT)
				continue;
			if (shown > 0)
				s += ", ";
			s += string::f("%d: %s", (int) i, names[i].c_str());
			shown++;
		}
		if (total > shown)
			s += string::f(", ... (%d more)", (int) (total - shown));
		if (total == 0)
			s = "(none)";
		return s;
	};

	if (!idJ || json_is_null(idJ)) {
		*err = string::f("Missing %s id. Use an integer index or a name. Available on %s: %s", kind.c_str(), owner.c_str(), listing().c_str());
		return false;
	}

	int64_t idx = -1;
	if (json_is_string(idJ)) {
		std::string s = trim(json_string_value(idJ));
		std::string sl = lowercase(s);
		std::vector<int> matches;
		for (size_t i = 0; i < names.size(); i++) {
			if (!names[i].empty() && lowercase(names[i]) == sl)
				matches.push_back((int) i);
		}
		if (matches.size() == 1) {
			*out = matches[0];
			return true;
		}
		if (matches.size() > 1) {
			std::string ids;
			for (int m : matches)
				ids += (ids.empty() ? "" : ", ") + std::to_string(m);
			*err = string::f("%s name '%s' is ambiguous on %s (matches ids %s). Use the numeric id. Available: %s", kind.c_str(), s.c_str(), owner.c_str(), ids.c_str(), listing().c_str());
			return false;
		}
		if (isDecimalString(s)) {
			char* end = NULL;
			long long v = strtoll(s.c_str(), &end, 10);
			idx = v;
		}
		else {
			*err = string::f("Unknown %s '%s' on %s. Available: %s", kind.c_str(), s.c_str(), owner.c_str(), listing().c_str());
			return false;
		}
	}
	else if (!jsonToIndex(idJ, &idx)) {
		*err = string::f("Invalid %s id: expected an integer index or a name. Available on %s: %s", kind.c_str(), owner.c_str(), listing().c_str());
		return false;
	}

	if (idx < 0 || idx >= (int64_t) names.size()) {
		*err = string::f("%s id %lld is out of range on %s (valid ids 0..%d). Available: %s", kind.c_str(), (long long) idx, owner.c_str(), (int) names.size() - 1, listing().c_str());
		return false;
	}
	if (names[idx].empty()) {
		*err = string::f("%s id %lld on %s cannot be used (not configured). Available: %s", kind.c_str(), (long long) idx, owner.c_str(), listing().c_str());
		return false;
	}
	*out = (int) idx;
	return true;
}


bool resolvePort(const ModuleRef& m, bool input, json_t* idJ, int* out, std::string* err) {
	size_t n = input ? m.module->inputs.size() : m.module->outputs.size();
	std::vector<std::string> names;
	for (size_t i = 0; i < n; i++) {
		// A port without widget cannot be cabled; hide it from the listing.
		bool hasWidget = input ? (m.mw->getInput((int) i) != NULL) : (m.mw->getOutput((int) i) != NULL);
		names.push_back(hasWidget ? portName(m.module, input, (int) i) : "");
	}
	std::string kind = input ? "Input" : "Output";
	return resolveId(kind, moduleDescription(m), names, idJ, out, err);
}


bool resolveParam(const ModuleRef& m, json_t* idJ, int* out, std::string* err) {
	std::vector<std::string> names;
	for (size_t i = 0; i < m.module->params.size(); i++) {
		engine::ParamQuantity* pq = paramQuantity(m.module, (int) i);
		names.push_back(pq ? pq->getLabel() : "");
	}
	return resolveId("Param", moduleDescription(m), names, idJ, out, err);
}


std::string displayString(engine::ParamQuantity* pq) {
	return pq->getDisplayValueString() + pq->getUnit();
}


// ---- Grid / placement ----------------------------------------------------------------

math::Vec gridToPx(int hp, int row) {
	return math::Vec((float) hp, (float) row).mult(app::RACK_GRID_SIZE).plus(app::RACK_OFFSET);
}


GridPos gridPosOf(app::ModuleWidget* mw) {
	math::Vec g = mw->getGridPosition();
	GridPos p;
	p.hp = (int) std::lround(g.x);
	p.row = (int) std::lround(g.y);
	return p;
}


int widthHp(app::ModuleWidget* mw) {
	int w = (int) std::lround(mw->box.size.x / app::RACK_GRID_WIDTH);
	return std::max(w, 1);
}


Occupancy::Occupancy(const app::ModuleWidget* ignore) {
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (mw == ignore)
			continue;
		Entry e;
		GridPos p = gridPosOf(mw);
		e.hp = p.hp;
		e.row = p.row;
		e.width = widthHp(mw);
		e.px = mw->box;
		entries.push_back(e);
	}
}


bool Occupancy::isFree(int hp, int row, int width) const {
	math::Rect cand(gridToPx(hp, row), math::Vec(width * app::RACK_GRID_WIDTH, app::RACK_GRID_HEIGHT));
	for (const Entry& e : entries) {
		if (cand.intersects(e.px))
			return false;
		if (e.row == row && hp < e.hp + e.width && e.hp < hp + width)
			return false;
	}
	return true;
}


int Occupancy::findFreeHp(int hp, int row, int width, bool rightOnly) const {
	int minLeft = std::min(hp, 0);
	int limit = 100000;
	for (int d = 0; d < limit; d++) {
		if (isFree(hp + d, row, width))
			return hp + d;
		if (!rightOnly && d > 0 && hp - d >= minLeft) {
			if (isFree(hp - d, row, width))
				return hp - d;
		}
	}
	// Unreachable in practice (the rack is unbounded to the right); fall back to past the last module.
	return std::max(hp, rowRightEdge(row, hp));
}


int Occupancy::rowRightEdge(int row, int none) const {
	bool any = false;
	int right = none;
	for (const Entry& e : entries) {
		if (e.row != row)
			continue;
		if (!any || e.hp + e.width > right)
			right = e.hp + e.width;
		any = true;
	}
	return right;
}


bool parsePosition(json_t* posJ, int* hp, int* row, std::string* err) {
	if (!posJ || !json_is_object(posJ)) {
		*err = "Invalid 'position': expected an object like {\"hp\": 10, \"row\": 0}.";
		return false;
	}
	int64_t h = 0, r = 0;
	if (!argInt64(posJ, "hp", &h)) {
		*err = "Invalid 'position': 'hp' (integer, 1 HP = 15 px horizontal grid unit) is required.";
		return false;
	}
	if (json_object_get(posJ, "row")) {
		if (!argInt64(posJ, "row", &r)) {
			*err = "Invalid 'position': 'row' must be an integer (0 = first row).";
			return false;
		}
	}
	if (h < -1000 || h > 10000) {
		*err = string::f("Invalid 'position': hp %lld is out of range (-1000..10000).", (long long) h);
		return false;
	}
	if (r < -50 || r > 50) {
		*err = string::f("Invalid 'position': row %lld is out of range (-50..50).", (long long) r);
		return false;
	}
	*hp = (int) h;
	*row = (int) r;
	return true;
}


// ---- History / cables ----------------------------------------------------------------

bool requireUndo(ToolContext& ctx, std::string* err) {
	if (!ctx.undo) {
		*err = "Internal error: no undo context available for this mutation.";
		return false;
	}
	return true;
}


int countModuleCables(const ModuleRef& m) {
	int n = 0;
	for (app::CableWidget* cw : APP->scene->rack->getCompleteCables()) {
		if (!cw->cable)
			continue;
		if (cw->cable->inputModule == m.module || cw->cable->outputModule == m.module)
			n++;
	}
	return n;
}


std::vector<app::CableWidget*> cablesOnInput(app::PortWidget* pw) {
	return APP->scene->rack->getCompleteCablesOnPort(pw);
}


int removeModuleWithHistory(app::ModuleWidget* mw, ToolContext& ctx) {
	size_t before = ctx.undo->actions.size();
	// Removes and records all cables first, so they are restored on undo
	mw->appendDisconnectActions(ctx.undo);
	int removedCables = (int) (ctx.undo->actions.size() - before);

	history::ModuleRemove* r = new history::ModuleRemove;
	try {
		r->setModule(mw);
	}
	catch (...) {
		delete r;
		throw;
	}
	ctx.undo->push(r);

	APP->scene->rack->removeModule(mw);
	delete mw;
	APP->scene->rack->updateExpanders();
	return removedCables;
}


std::string endLabel(engine::Module* module, bool input, int idx) {
	ModuleRef m;
	m.module = module;
	return moduleName(m) + " " + portName(module, input, idx);
}


} // namespace helpers
} // namespace assistant
} // namespace rack

#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cerrno>

#include <string.hpp>


namespace rack {
namespace assistant {


using namespace helpers;


// ---- Argument helpers ----------------------------------------------------------------

static json_t* getArg(json_t* args, const char* key) {
	if (!args || !key || !json_is_object(args))
		return NULL;
	return json_object_get(args, key);
}


/** Parses a complete decimal number (surrounding whitespace allowed). */
static bool parseNumber(const std::string& str, double* out) {
	std::string s = trim(str);
	if (s.empty())
		return false;
	errno = 0;
	char* end = NULL;
	double v = strtod(s.c_str(), &end);
	if (end == s.c_str() || *end != '\0')
		return false;
	if (!std::isfinite(v))
		return false;
	*out = v;
	return true;
}


bool argInt64(json_t* args, const char* key, int64_t* out) {
	json_t* j = getArg(args, key);
	if (!j || !out)
		return false;
	if (json_is_integer(j)) {
		*out = (int64_t) json_integer_value(j);
		return true;
	}
	if (json_is_real(j)) {
		double v = json_real_value(j);
		if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 9.007199254740992e15) {
			*out = (int64_t) v;
			return true;
		}
		return false;
	}
	if (json_is_string(j)) {
		std::string s = trim(json_string_value(j));
		if (s.empty())
			return false;
		// Plain integer text: parse exactly (no double rounding for large ids)
		errno = 0;
		char* end = NULL;
		long long v = strtoll(s.c_str(), &end, 10);
		if (end != s.c_str() && *end == '\0' && errno == 0) {
			*out = (int64_t) v;
			return true;
		}
		// Integral real text such as "12.0" or "1e3"
		double d;
		if (parseNumber(s, &d) && std::floor(d) == d && std::fabs(d) < 9.007199254740992e15) {
			*out = (int64_t) d;
			return true;
		}
	}
	return false;
}


bool argFloat(json_t* args, const char* key, float* out) {
	json_t* j = getArg(args, key);
	if (!j || !out)
		return false;
	double v = 0.0;
	if (json_is_integer(j) || json_is_real(j)) {
		v = json_number_value(j);
		if (!std::isfinite(v))
			return false;
	}
	else if (json_is_string(j)) {
		if (!parseNumber(json_string_value(j), &v))
			return false;
	}
	else {
		return false;
	}
	// Reject values outside float range: the cast would yield +/-inf
	if (std::fabs(v) > (double) FLT_MAX)
		return false;
	*out = (float) v;
	return true;
}


bool argString(json_t* args, const char* key, std::string* out) {
	json_t* j = getArg(args, key);
	if (!j || !out || !json_is_string(j))
		return false;
	const char* s = json_string_value(j);
	size_t len = json_string_length(j);
	*out = std::string(s ? s : "", s ? len : 0);
	return true;
}


bool argBool(json_t* args, const char* key, bool* out) {
	json_t* j = getArg(args, key);
	if (!j || !out)
		return false;
	if (json_is_boolean(j)) {
		*out = json_is_true(j);
		return true;
	}
	if (json_is_string(j)) {
		std::string s = lowercase(trim(json_string_value(j)));
		if (s == "true") {
			*out = true;
			return true;
		}
		if (s == "false") {
			*out = false;
			return true;
		}
	}
	return false;
}


std::string okJson(json_t* obj) {
	JsonPtr o(obj);
	if (!o || !json_is_object(o.get()))
		o.reset(json_object());
	json_object_set_new(o.get(), "ok", json_true());
	std::string s = dumpCompact(o.get());
	if (s.empty())
		return errorJson("Internal error: could not serialize the result.");
	return s;
}


std::string errorJson(const std::string& msg) {
	JsonPtr o(json_object());
	json_object_set_new(o.get(), "ok", json_false());
	json_object_set_new(o.get(), "error", jStr(msg));
	std::string s = dumpCompact(o.get());
	if (s.empty())
		return "{\"ok\":false,\"error\":\"Internal error\"}";
	return s;
}


ToolResult errorResult(const std::string& msg) {
	ToolResult r;
	r.ok = false;
	r.content = errorJson(msg);
	r.summary = msg;
	r.mutated = false;
	return r;
}


// ---- ToolRegistry --------------------------------------------------------------------

void ToolRegistry::add(const Tool& t) {
	// Replace a tool with the same name
	for (Tool& existing : tools) {
		if (existing.name == t.name) {
			existing = t;
			return;
		}
	}
	tools.push_back(t);
}


const Tool* ToolRegistry::find(const std::string& name) const {
	for (const Tool& t : tools) {
		if (t.name == name)
			return &t;
	}
	return NULL;
}


const std::vector<Tool>& ToolRegistry::all() const {
	return tools;
}


std::string ToolRegistry::toolsJson() const {
	JsonPtr arr(json_array());
	for (const Tool& t : tools) {
		json_t* fn = json_object();
		setStr(fn, "name", t.name);
		setStr(fn, "description", t.description);
		json_error_t error;
		json_t* params = json_loads(t.parametersSchema.c_str(), 0, &error);
		if (!params || !json_is_object(params)) {
			if (params)
				json_decref(params);
			params = json_pack("{s:s,s:{}}", "type", "object", "properties");
		}
		setJson(fn, "parameters", params);
		json_t* item = json_object();
		setStr(item, "type", "function");
		setJson(item, "function", fn);
		appendJson(arr.get(), item);
	}
	return dumpCompact(arr.get());
}


/** Parses tool arguments text into a JSON object. Returns NULL and fills err on failure. */
static json_t* parseArgs(const std::string& argsJson, std::string* err) {
	std::string text = trim(argsJson);
	if (text.empty())
		text = "{}";
	json_error_t error;
	json_t* args = json_loads(text.c_str(), 0, &error);
	if (!args) {
		*err = string::f("Invalid JSON arguments (%s at line %d column %d). Send a single JSON object.", error.text, error.line, error.column);
		return NULL;
	}
	if (!json_is_object(args)) {
		json_decref(args);
		*err = "Invalid arguments: expected a JSON object.";
		return NULL;
	}
	return args;
}


ToolResult ToolRegistry::execute(const std::string& name, const std::string& argsJson, ToolContext& ctx) const {
	const Tool* tool = find(name);
	if (!tool || !tool->run) {
		std::string names;
		for (const Tool& t : tools)
			names += (names.empty() ? "" : ", ") + t.name;
		return errorResult(string::f("Unknown tool '%s'. Available tools: %s.", name.c_str(), names.c_str()));
	}

	std::string err;
	JsonPtr args(parseArgs(argsJson, &err));
	if (!args)
		return errorResult(err);

	ToolResult result;
	try {
		result = tool->run(args.get(), ctx);
	}
	catch (const std::exception& e) {
		// State may be partially modified; mutations already recorded in ctx.undo stay undoable.
		return errorResult(string::f("Tool '%s' failed: %s", name.c_str(), e.what()));
	}
	catch (...) {
		return errorResult(string::f("Tool '%s' failed with an unknown error.", name.c_str()));
	}

	if (result.content.empty()) {
		result.content = result.ok ? okJson(NULL) : errorJson(result.summary.empty() ? "Unknown error." : result.summary);
	}
	if (result.mutated)
		ctx.mutationsSinceSave++;
	return result;
}


std::string ToolRegistry::confirmationFor(const std::string& name, const std::string& argsJson) const {
	const Tool* tool = find(name);
	if (!tool || !tool->confirmation)
		return "";
	std::string err;
	JsonPtr args(parseArgs(argsJson, &err));
	if (!args)
		return "";
	try {
		return tool->confirmation(args.get());
	}
	catch (...) {
		return "";
	}
}


ToolRegistry& defaultRegistry() {
	static ToolRegistry registry;
	static bool initialized = false;
	if (!initialized) {
		initialized = true;
		registerCatalogTools(registry);
		registerPatchTools(registry);
	}
	return registry;
}


} // namespace assistant
} // namespace rack

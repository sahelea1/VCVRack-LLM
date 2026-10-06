// Unit tests for the tool argument helpers and result builders (no APP / no UI needed).
#include "test.hpp"

#include <assistant/Tools.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include <jansson.h>


using namespace rack::assistant;


namespace {

struct Json {
	json_t* j;
	explicit Json(const char* text) {
		json_error_t err;
		j = json_loads(text, 0, &err);
	}
	~Json() {
		if (j)
			json_decref(j);
	}
	json_t* get() const {
		return j;
	}
};

} // namespace


TEST(args_int64_accepts_integers) {
	Json a("{\"id\": 123, \"neg\": -7, \"big\": 9007199254740991, \"zero\": 0}");
	CHECK(a.get() != NULL);
	int64_t v = 0;
	CHECK(argInt64(a.get(), "id", &v));
	CHECK_EQ(v, 123);
	CHECK(argInt64(a.get(), "neg", &v));
	CHECK_EQ(v, -7);
	CHECK(argInt64(a.get(), "big", &v));
	CHECK_EQ(v, 9007199254740991LL);
	CHECK(argInt64(a.get(), "zero", &v));
	CHECK_EQ(v, 0);
}

TEST(args_int64_accepts_integral_reals_and_strings) {
	Json a("{\"r\": 5.0, \"s\": \"42\", \"sp\": \" 42 \", \"sneg\": \"-9\", \"big\": \"9007199254740991\", \"sr\": \"12.0\"}");
	int64_t v = 0;
	CHECK(argInt64(a.get(), "r", &v));
	CHECK_EQ(v, 5);
	CHECK(argInt64(a.get(), "s", &v));
	CHECK_EQ(v, 42);
	CHECK(argInt64(a.get(), "sp", &v));
	CHECK_EQ(v, 42);
	CHECK(argInt64(a.get(), "sneg", &v));
	CHECK_EQ(v, -9);
	CHECK(argInt64(a.get(), "big", &v));
	CHECK_EQ(v, 9007199254740991LL);
	CHECK(argInt64(a.get(), "sr", &v));
	CHECK_EQ(v, 12);
}

TEST(args_int64_rejects_bad_values) {
	Json a("{\"frac\": 5.5, \"str\": \"abc\", \"empty\": \"\", \"trail\": \"4x\", \"b\": true, \"n\": null, \"arr\": [1], \"obj\": {}, \"huge\": 1e300, \"sfrac\": \"1.5\"}");
	int64_t v = 777;
	CHECK(!argInt64(a.get(), "frac", &v));
	CHECK(!argInt64(a.get(), "str", &v));
	CHECK(!argInt64(a.get(), "empty", &v));
	CHECK(!argInt64(a.get(), "trail", &v));
	CHECK(!argInt64(a.get(), "b", &v));
	CHECK(!argInt64(a.get(), "n", &v));
	CHECK(!argInt64(a.get(), "arr", &v));
	CHECK(!argInt64(a.get(), "obj", &v));
	CHECK(!argInt64(a.get(), "huge", &v));
	CHECK(!argInt64(a.get(), "sfrac", &v));
	CHECK(!argInt64(a.get(), "missing", &v));
	CHECK_EQ(v, 777);
}

TEST(args_helpers_tolerate_null_and_non_objects) {
	int64_t i = 0;
	float f = 0.f;
	std::string s;
	bool b = false;
	CHECK(!argInt64(NULL, "k", &i));
	CHECK(!argFloat(NULL, "k", &f));
	CHECK(!argString(NULL, "k", &s));
	CHECK(!argBool(NULL, "k", &b));
	Json arr("[1,2,3]");
	CHECK(!argInt64(arr.get(), "k", &i));
	CHECK(!argString(arr.get(), "k", &s));
	Json obj("{\"k\": 1}");
	CHECK(!argInt64(obj.get(), NULL, &i));
	CHECK(!argInt64(obj.get(), "k", NULL));
}

TEST(args_float) {
	Json a("{\"i\": 3, \"r\": 0.25, \"neg\": -1.5, \"s\": \"2.5\", \"sp\": \" 1e2 \", \"bad\": \"x\", \"b\": false, \"n\": null, \"empty\": \"\", \"inf\": \"inf\", \"nan\": \"nan\", \"big\": 1e300, \"sbig\": \"1e300\", \"nbig\": -1e40}");
	float v = 0.f;
	CHECK(argFloat(a.get(), "i", &v));
	CHECK_EQ(v, 3.f);
	CHECK(argFloat(a.get(), "r", &v));
	CHECK_EQ(v, 0.25f);
	CHECK(argFloat(a.get(), "neg", &v));
	CHECK_EQ(v, -1.5f);
	CHECK(argFloat(a.get(), "s", &v));
	CHECK_EQ(v, 2.5f);
	CHECK(argFloat(a.get(), "sp", &v));
	CHECK_EQ(v, 100.f);
	v = 9.f;
	CHECK(!argFloat(a.get(), "bad", &v));
	CHECK(!argFloat(a.get(), "b", &v));
	CHECK(!argFloat(a.get(), "n", &v));
	CHECK(!argFloat(a.get(), "empty", &v));
	CHECK(!argFloat(a.get(), "inf", &v));
	CHECK(!argFloat(a.get(), "nan", &v));
	CHECK(!argFloat(a.get(), "big", &v));
	CHECK(!argFloat(a.get(), "sbig", &v));
	CHECK(!argFloat(a.get(), "nbig", &v));
	CHECK(!argFloat(a.get(), "missing", &v));
	CHECK_EQ(v, 9.f);
}

TEST(args_string) {
	Json a("{\"s\": \"hello\", \"empty\": \"\", \"utf\": \"caf\\u00e9\", \"i\": 5, \"n\": null, \"b\": true}");
	std::string s;
	CHECK(argString(a.get(), "s", &s));
	CHECK_EQ(s, "hello");
	CHECK(argString(a.get(), "empty", &s));
	CHECK_EQ(s, "");
	CHECK(argString(a.get(), "utf", &s));
	CHECK_EQ(s, "caf\xC3\xA9");
	s = "keep";
	CHECK(!argString(a.get(), "i", &s));
	CHECK(!argString(a.get(), "n", &s));
	CHECK(!argString(a.get(), "b", &s));
	CHECK(!argString(a.get(), "missing", &s));
	CHECK_EQ(s, "keep");
}

TEST(args_bool) {
	Json a("{\"t\": true, \"f\": false, \"st\": \"true\", \"sf\": \"False\", \"sp\": \" TRUE \", \"yes\": \"yes\", \"one\": 1, \"n\": null}");
	bool v = false;
	CHECK(argBool(a.get(), "t", &v));
	CHECK_EQ(v, true);
	CHECK(argBool(a.get(), "f", &v));
	CHECK_EQ(v, false);
	CHECK(argBool(a.get(), "st", &v));
	CHECK_EQ(v, true);
	CHECK(argBool(a.get(), "sf", &v));
	CHECK_EQ(v, false);
	CHECK(argBool(a.get(), "sp", &v));
	CHECK_EQ(v, true);
	v = true;
	CHECK(!argBool(a.get(), "yes", &v));
	CHECK(!argBool(a.get(), "one", &v));
	CHECK(!argBool(a.get(), "n", &v));
	CHECK(!argBool(a.get(), "missing", &v));
	CHECK_EQ(v, true);
}

TEST(args_okjson_shape) {
	json_t* obj = json_object();
	json_object_set_new(obj, "module_id", json_integer(42));
	json_object_set_new(obj, "name", json_string("VCO"));
	std::string text = okJson(obj); // steals obj
	Json parsed(text.c_str());
	CHECK(parsed.get() != NULL);
	CHECK(json_is_object(parsed.get()));
	CHECK(json_is_true(json_object_get(parsed.get(), "ok")));
	CHECK_EQ((int) json_integer_value(json_object_get(parsed.get(), "module_id")), 42);
	CHECK_EQ(std::string(json_string_value(json_object_get(parsed.get(), "name"))), "VCO");
	// Compact: no newlines or indentation
	CHECK(text.find('\n') == std::string::npos);
	CHECK(text.find(": ") == std::string::npos);
}

TEST(args_okjson_overrides_and_handles_null) {
	json_t* obj = json_object();
	json_object_set_new(obj, "ok", json_false());
	Json parsed(okJson(obj).c_str());
	CHECK(json_is_true(json_object_get(parsed.get(), "ok")));

	Json empty(okJson(NULL).c_str());
	CHECK(json_is_object(empty.get()));
	CHECK(json_is_true(json_object_get(empty.get(), "ok")));
	CHECK_EQ(json_object_size(empty.get()), (size_t) 1);

	// Not an object: replaced by an empty ok object
	Json fromArray(okJson(json_array()).c_str());
	CHECK(json_is_true(json_object_get(fromArray.get(), "ok")));
}

TEST(args_okjson_numbers_are_compact) {
	json_t* obj = json_object();
	json_object_set_new(obj, "v", json_real(0.1));
	std::string text = okJson(obj);
	CHECK(text.find("0.1") != std::string::npos);
	CHECK(text.find("0.100000") == std::string::npos);
}

TEST(args_errorjson_shape) {
	std::string text = errorJson("Module id 5 not found. Call \"get_patch\".");
	Json parsed(text.c_str());
	CHECK(parsed.get() != NULL);
	CHECK(json_is_false(json_object_get(parsed.get(), "ok")));
	CHECK_EQ(std::string(json_string_value(json_object_get(parsed.get(), "error"))), "Module id 5 not found. Call \"get_patch\".");
	CHECK_EQ(json_object_size(parsed.get()), (size_t) 2);
}

TEST(args_errorjson_survives_invalid_utf8) {
	std::string bad = "bad \xFF\xFE bytes \xC3";
	std::string text = errorJson(bad);
	Json parsed(text.c_str());
	CHECK(parsed.get() != NULL);
	CHECK(json_is_false(json_object_get(parsed.get(), "ok")));
	CHECK(json_is_string(json_object_get(parsed.get(), "error")));
}

TEST(args_errorresult) {
	ToolResult r = errorResult("nope");
	CHECK(!r.ok);
	CHECK(!r.mutated);
	CHECK_EQ(r.summary, "nope");
	Json parsed(r.content.c_str());
	CHECK(json_is_false(json_object_get(parsed.get(), "ok")));
	CHECK_EQ(std::string(json_string_value(json_object_get(parsed.get(), "error"))), "nope");
}

TEST(args_registry_basics_without_app) {
	ToolRegistry reg;
	Tool t;
	t.name = "echo";
	t.description = "Echo";
	t.parametersSchema = "{\"type\":\"object\",\"properties\":{}}";
	t.run = [](json_t* args, ToolContext& ctx) {
		ToolResult r;
		json_t* o = json_object();
		json_object_set_new(o, "n", json_integer(1));
		r.content = okJson(o);
		r.mutated = true;
		return r;
	};
	reg.add(t);
	CHECK(reg.find("echo") != NULL);
	CHECK(reg.find("other") == NULL);
	CHECK_EQ(reg.all().size(), (size_t) 1);

	ToolContext ctx;
	ToolResult r = reg.execute("echo", "", ctx);
	CHECK(r.ok);
	CHECK_EQ(ctx.mutationsSinceSave, 1);
	r = reg.execute("echo", "{bad", ctx);
	CHECK(!r.ok);
	CHECK_EQ(ctx.mutationsSinceSave, 1);
	r = reg.execute("echo", "[]", ctx);
	CHECK(!r.ok);
	r = reg.execute("nope", "{}", ctx);
	CHECK(!r.ok);
	CHECK(reg.confirmationFor("echo", "{}").empty());

	// Exceptions inside tools become error results
	Tool bad;
	bad.name = "boom";
	bad.run = [](json_t* args, ToolContext& ctx) -> ToolResult {
		throw std::runtime_error("kaboom");
	};
	reg.add(bad);
	r = reg.execute("boom", "{}", ctx);
	CHECK(!r.ok);
	CHECK(r.content.find("kaboom") != std::string::npos);

	// Destructive tool confirmation
	Tool del;
	del.name = "del";
	del.confirmation = [](json_t* args) -> std::string {
		return "Really?";
	};
	del.run = [](json_t* args, ToolContext& ctx) {
		return ToolResult();
	};
	reg.add(del);
	CHECK_EQ(reg.confirmationFor("del", "{}"), "Really?");
	CHECK_EQ(reg.confirmationFor("del", "{bad"), "");

	// toolsJson: valid array; invalid schema falls back to an object schema
	Json tools(reg.toolsJson().c_str());
	CHECK(json_is_array(tools.get()));
	CHECK_EQ(json_array_size(tools.get()), (size_t) 3);
	json_t* fn = json_object_get(json_array_get(tools.get(), 0), "function");
	CHECK_EQ(std::string(json_string_value(json_object_get(fn, "name"))), "echo");
	CHECK_EQ(std::string(json_string_value(json_object_get(json_object_get(fn, "parameters"), "type"))), "object");
	json_t* fn2 = json_object_get(json_array_get(tools.get(), 1), "function");
	CHECK_EQ(std::string(json_string_value(json_object_get(json_object_get(fn2, "parameters"), "type"))), "object");
}

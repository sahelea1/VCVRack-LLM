#include "test.hpp"

#include <assistant/Config.hpp>
#include <asset.hpp>
#include <system.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>


using namespace rack;
using namespace rack::assistant;


namespace {

struct JsonPtr {
	json_t* j;
	explicit JsonPtr(json_t* j) : j(j) {}
	~JsonPtr() {
		if (j)
			json_decref(j);
	}
};


json_t* parse(const std::string& s) {
	json_error_t error;
	return json_loads(s.c_str(), 0, &error);
}


/** Parses a config from JSON text, collecting warnings. */
Config fromText(const std::string& text, std::vector<std::string>* warnings = NULL) {
	JsonPtr j(parse(text));
	return configFromJson(j.j, warnings);
}


/** A fresh directory below the test's temp user dir. */
std::string makeTempDir(const std::string& name) {
	std::string dir = system::join(asset::user(""), "config-test-" + name);
	system::removeRecursively(dir);
	system::createDirectories(dir);
	return dir;
}


std::string readFile(const std::string& path) {
	std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}


void writeFile(const std::string& path, const std::string& text) {
	std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	f << text;
}


int fileMode(const std::string& path) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return -1;
	return (int) (st.st_mode & 0777);
}


/** Sets/unsets an env var for the scope of a test. */
struct EnvGuard {
	std::string name;
	bool hadValue;
	std::string oldValue;
	explicit EnvGuard(const char* name) : name(name) {
		const char* v = std::getenv(name);
		hadValue = (v != NULL);
		if (v)
			oldValue = v;
	}
	~EnvGuard() {
		if (hadValue)
			setenv(name.c_str(), oldValue.c_str(), 1);
		else
			unsetenv(name.c_str());
	}
};

} // namespace


TEST(config_defaults) {
	Config c = defaultConfig();
	CHECK_EQ(c.baseUrl, "https://openrouter.ai/api/v1");
	CHECK_EQ(c.model, "openai/gpt-5.6-terra");
	CHECK_EQ(c.reasoningEffort, "medium");
	CHECK_EQ(c.reasoningParamStyle, "openrouter");
	CHECK_EQ(c.maxTokens, 0);
	CHECK(!c.hasTemperature);
	CHECK(c.confirmDestructive);
	CHECK_EQ(c.maxToolRounds, 15);
	CHECK(c.timeoutSec == 180.0);
	CHECK_EQ(c.caBundle, "");
	CHECK(!c.mock);
	CHECK_EQ(c.apiKey, "");
	CHECK(c.panelWidth == 400.f);
	CHECK(c.attachSelection);
	CHECK_EQ(c.maxContextChars, 200000);
	REQUIRE(c.extraHeaders.size() == 2);
	CHECK_EQ(c.extraHeaders[0].first, "HTTP-Referer");
	CHECK_EQ(c.extraHeaders[0].second, "https://github.com/sahelea1/vcvrack-llm");
	CHECK_EQ(c.extraHeaders[1].first, "X-Title");
	CHECK_EQ(c.extraHeaders[1].second, "VCV Rack Assistant");
	CHECK(configPath().size() > 0);
	CHECK(configPath().find("assistant.json") != std::string::npos);
}

TEST(config_empty_object_is_defaults) {
	std::vector<std::string> w;
	Config c = fromText("{}", &w);
	CHECK(w.empty());
	CHECK_EQ(c.model, defaultConfig().model);
	CHECK_EQ((int) c.extraHeaders.size(), 2);
}

TEST(config_parse_all_fields) {
	std::vector<std::string> w;
	Config c = fromText(R"({
		"base_url": "https://api.openai.com/v1",
		"model": "gpt-x",
		"reasoning_effort": "high",
		"reasoning_param_style": "openai",
		"max_tokens": 4096,
		"temperature": 0.25,
		"extra_headers": {"X-A": "1", "X-B": "two"},
		"confirm_destructive": false,
		"max_tool_rounds": 7,
		"timeout_sec": 60,
		"ca_bundle": "/tmp/ca.pem",
		"mock": true,
		"api_key": "secret-key-value",
		"panel_width": 500,
		"attach_selection": false,
		"max_context_chars": 50000
	})", &w);
	CHECK(w.empty());
	CHECK_EQ(c.baseUrl, "https://api.openai.com/v1");
	CHECK_EQ(c.model, "gpt-x");
	CHECK_EQ(c.reasoningEffort, "high");
	CHECK_EQ(c.reasoningParamStyle, "openai");
	CHECK_EQ(c.maxTokens, 4096);
	CHECK(c.hasTemperature);
	CHECK(c.temperature == 0.25f);
	REQUIRE(c.extraHeaders.size() == 2);
	CHECK_EQ(c.extraHeaders[0].first, "X-A");
	CHECK_EQ(c.extraHeaders[1].second, "two");
	CHECK(!c.confirmDestructive);
	CHECK_EQ(c.maxToolRounds, 7);
	CHECK(c.timeoutSec == 60.0);
	CHECK_EQ(c.caBundle, "/tmp/ca.pem");
	CHECK(c.mock);
	CHECK_EQ(c.apiKey, "secret-key-value");
	CHECK(c.panelWidth == 500.f);
	CHECK(!c.attachSelection);
	CHECK_EQ(c.maxContextChars, 50000);
}

TEST(config_max_tokens_and_temperature_unset_forms) {
	std::vector<std::string> w;
	CHECK_EQ(fromText("{\"max_tokens\":null}", &w).maxTokens, 0);
	CHECK_EQ(fromText("{\"max_tokens\":0}", &w).maxTokens, 0);
	CHECK_EQ(fromText("{\"max_tokens\":\"\"}", &w).maxTokens, 0);
	CHECK_EQ(fromText("{\"max_tokens\":-5}", &w).maxTokens, 0);
	CHECK(!fromText("{\"temperature\":null}", &w).hasTemperature);
	CHECK(!fromText("{\"temperature\":\"\"}", &w).hasTemperature);
	CHECK(w.empty());
	Config c = fromText("{\"temperature\":0}", &w);
	CHECK(c.hasTemperature);
	CHECK(c.temperature == 0.f);
	CHECK(w.empty());
}

TEST(config_invalid_values_keep_defaults_with_warnings) {
	std::vector<std::string> w;
	Config c = fromText(R"({
		"base_url": 5,
		"model": ["a"],
		"reasoning_effort": "extreme",
		"reasoning_param_style": "azure",
		"max_tokens": "lots",
		"temperature": "hot",
		"extra_headers": "nope",
		"confirm_destructive": "yes",
		"max_tool_rounds": "many",
		"timeout_sec": true,
		"ca_bundle": 1,
		"mock": 1,
		"api_key": 12,
		"panel_width": {},
		"attach_selection": null,
		"max_context_chars": []
	})", &w);
	Config d = defaultConfig();
	CHECK_EQ(c.baseUrl, d.baseUrl);
	CHECK_EQ(c.model, d.model);
	CHECK_EQ(c.reasoningEffort, d.reasoningEffort);
	CHECK_EQ(c.reasoningParamStyle, d.reasoningParamStyle);
	CHECK_EQ(c.maxTokens, 0);
	CHECK(!c.hasTemperature);
	CHECK_EQ((int) c.extraHeaders.size(), 2);
	CHECK_EQ(c.confirmDestructive, d.confirmDestructive);
	CHECK_EQ(c.maxToolRounds, d.maxToolRounds);
	CHECK(c.timeoutSec == d.timeoutSec);
	CHECK_EQ(c.caBundle, "");
	CHECK(!c.mock);
	CHECK_EQ(c.apiKey, "");
	CHECK(c.panelWidth == d.panelWidth);
	CHECK_EQ(c.maxContextChars, d.maxContextChars);
	// attach_selection: null is not a boolean
	CHECK(c.attachSelection);
	CHECK(w.size() >= 15);
}

TEST(config_clamping) {
	std::vector<std::string> w;
	Config c = fromText("{\"max_tool_rounds\":0,\"timeout_sec\":1,\"panel_width\":10}", &w);
	CHECK_EQ(c.maxToolRounds, 1);
	CHECK(c.timeoutSec == 10.0);
	CHECK(c.panelWidth == 280.f);
	CHECK_EQ((int) w.size(), 3);
	w.clear();
	Config d = fromText("{\"max_tool_rounds\":1000,\"timeout_sec\":99999,\"panel_width\":99999}", &w);
	CHECK_EQ(d.maxToolRounds, 100);
	CHECK(d.timeoutSec == 1800.0);
	CHECK(d.panelWidth == 1200.f);
	CHECK_EQ((int) w.size(), 3);
	// Integral reals are accepted for ints
	Config e = fromText("{\"max_tool_rounds\":9.0}");
	CHECK_EQ(e.maxToolRounds, 9);
}

TEST(config_extra_headers_validation) {
	std::vector<std::string> w;
	Config c = fromText("{\"extra_headers\":{\"Good\":\"v\",\"Bad\\r\\nName\":\"v\",\"Evil\":\"a\\r\\nX-Injected: 1\",\"NewlineOnly\":\"a\\nb\",\"Num\":5,\"\":\"empty name\",\"Co:lon\":\"x\"}}", &w);
	REQUIRE(c.extraHeaders.size() == 1);
	CHECK_EQ(c.extraHeaders[0].first, "Good");
	CHECK(w.size() >= 6);
	// An empty object removes the default headers
	Config d = fromText("{\"extra_headers\":{}}");
	CHECK(d.extraHeaders.empty());
	// null keeps the defaults
	Config e = fromText("{\"extra_headers\":null}");
	CHECK_EQ((int) e.extraHeaders.size(), 2);
}

TEST(config_non_object_root) {
	std::vector<std::string> w;
	JsonPtr j(parse("[1,2]"));
	Config c = configFromJson(j.j, &w);
	CHECK_EQ(c.model, defaultConfig().model);
	CHECK_EQ((int) w.size(), 1);
	std::vector<std::string> w2;
	Config d = configFromJson(NULL, &w2);
	CHECK_EQ(d.model, defaultConfig().model);
	CHECK_EQ((int) w2.size(), 1);
	// warnings pointer is optional
	configFromJson(NULL);
}

TEST(config_to_json) {
	Config c = defaultConfig();
	JsonPtr j(configToJson(c));
	REQUIRE(json_is_object(j.j));
	CHECK(!json_object_get(j.j, "api_key"));
	CHECK(json_is_null(json_object_get(j.j, "max_tokens")));
	CHECK(json_is_null(json_object_get(j.j, "temperature")));
	json_t* h = json_object_get(j.j, "extra_headers");
	REQUIRE(json_is_object(h));
	CHECK_EQ(std::string(json_string_value(json_object_get(h, "X-Title"))), "VCV Rack Assistant");

	c.apiKey = "abc";
	c.maxTokens = 99;
	c.hasTemperature = true;
	c.temperature = 0.75f;
	JsonPtr k(configToJson(c));
	CHECK_EQ(std::string(json_string_value(json_object_get(k.j, "api_key"))), "abc");
	CHECK_EQ((int) json_integer_value(json_object_get(k.j, "max_tokens")), 99);
	CHECK(json_real_value(json_object_get(k.j, "temperature")) == 0.75);
}

TEST(config_save_load_roundtrip_and_permissions) {
	std::string dir = makeTempDir("roundtrip");
	std::string path = system::join(dir, "assistant.json");

	Config c = defaultConfig();
	c.baseUrl = "http://localhost:11434/v1";
	c.model = "llama\xC3\xA4";
	c.reasoningEffort = "off";
	c.reasoningParamStyle = "openai";
	c.maxTokens = 2048;
	c.hasTemperature = true;
	c.temperature = 0.5f;
	c.extraHeaders.clear();
	c.extraHeaders.push_back(std::make_pair("X-One", "1"));
	c.confirmDestructive = false;
	c.maxToolRounds = 33;
	c.timeoutSec = 90.0;
	c.caBundle = "/etc/ca.pem";
	c.mock = true;
	c.apiKey = "test-not-a-real-key-123456";
	c.panelWidth = 640.f;
	c.attachSelection = false;
	c.maxContextChars = 12345;

	std::string error;
	CHECK(saveConfig(c, path, &error));
	CHECK_EQ(error, "");
	CHECK_EQ(fileMode(path), 0600);
	// The temp file is gone after the atomic rename
	CHECK(!system::exists(path + ".tmp"));

	std::vector<std::string> w;
	Config d = loadConfig(path, &w);
	CHECK(w.empty());
	CHECK_EQ(d.baseUrl, c.baseUrl);
	CHECK_EQ(d.model, c.model);
	CHECK_EQ(d.reasoningEffort, "off");
	CHECK_EQ(d.reasoningParamStyle, "openai");
	CHECK_EQ(d.maxTokens, 2048);
	CHECK(d.hasTemperature);
	CHECK(d.temperature == 0.5f);
	REQUIRE(d.extraHeaders.size() == 1);
	CHECK_EQ(d.extraHeaders[0].first, "X-One");
	CHECK(!d.confirmDestructive);
	CHECK_EQ(d.maxToolRounds, 33);
	CHECK(d.timeoutSec == 90.0);
	CHECK_EQ(d.caBundle, "/etc/ca.pem");
	CHECK(d.mock);
	CHECK_EQ(d.apiKey, "test-not-a-real-key-123456");
	CHECK(d.panelWidth == 640.f);
	CHECK(!d.attachSelection);
	CHECK_EQ(d.maxContextChars, 12345);

	// Overwriting an existing file keeps 0600 even if someone loosened the mode
	chmod(path.c_str(), 0644);
	CHECK(saveConfig(c, path));
	CHECK_EQ(fileMode(path), 0600);

	system::removeRecursively(dir);
}

TEST(config_save_creates_missing_directory) {
	std::string dir = makeTempDir("mkdir");
	std::string path = system::join(dir, "nested/deeper/assistant.json");
	std::string error;
	CHECK(saveConfig(defaultConfig(), path, &error));
	CHECK(system::isFile(path));
	system::removeRecursively(dir);
}

TEST(config_save_failure_reports_error) {
	std::string dir = makeTempDir("fail");
	// A regular file where a directory is needed
	std::string blocker = system::join(dir, "blocker");
	writeFile(blocker, "x");
	std::string error;
	CHECK(!saveConfig(defaultConfig(), system::join(blocker, "assistant.json"), &error));
	CHECK(!error.empty());
	system::removeRecursively(dir);
}

TEST(config_load_missing_file_writes_defaults_without_key) {
	std::string dir = makeTempDir("missing");
	std::string path = system::join(dir, "assistant.json");
	CHECK(!system::exists(path));
	std::vector<std::string> w;
	Config c = loadConfig(path, &w);
	CHECK(w.empty());
	CHECK_EQ(c.model, defaultConfig().model);
	REQUIRE(system::isFile(path));
	CHECK_EQ(fileMode(path), 0600);
	std::string text = readFile(path);
	CHECK(text.find("api_key") == std::string::npos);
	CHECK(text.find("base_url") != std::string::npos);
	// The written file parses back to the defaults
	std::vector<std::string> w2;
	Config d = loadConfig(path, &w2);
	CHECK(w2.empty());
	CHECK_EQ(d.baseUrl, c.baseUrl);
	system::removeRecursively(dir);
}

TEST(config_load_bad_json_returns_defaults_and_leaves_file) {
	std::string dir = makeTempDir("badjson");
	std::string path = system::join(dir, "assistant.json");
	writeFile(path, "{ this is not json");
	std::vector<std::string> w;
	Config c = loadConfig(path, &w);
	CHECK_EQ(c.model, defaultConfig().model);
	CHECK_EQ((int) w.size(), 1);
	CHECK_EQ(readFile(path), "{ this is not json");
	// no warnings pointer: must not crash
	loadConfig(path);
	system::removeRecursively(dir);
}

TEST(config_load_directory_does_not_throw) {
	std::string dir = makeTempDir("isdir");
	std::vector<std::string> w;
	Config c = loadConfig(dir, &w);
	CHECK_EQ(c.model, defaultConfig().model);
	system::removeRecursively(dir);
}

TEST(config_load_partial_file_with_type_errors) {
	std::string dir = makeTempDir("partial");
	std::string path = system::join(dir, "assistant.json");
	writeFile(path, "{\"model\":\"m1\",\"max_tool_rounds\":\"x\",\"unknown_key\":true}");
	std::vector<std::string> w;
	Config c = loadConfig(path, &w);
	CHECK_EQ(c.model, "m1");
	CHECK_EQ(c.maxToolRounds, 15);
	CHECK_EQ((int) w.size(), 1);
	system::removeRecursively(dir);
}


// API key

TEST(key_precedence) {
	EnvGuard g1("RACK_ASSISTANT_API_KEY");
	EnvGuard g2("OPENROUTER_API_KEY");
	Config c;
	c.apiKey = "file-key";

	unsetenv("RACK_ASSISTANT_API_KEY");
	unsetenv("OPENROUTER_API_KEY");
	ResolvedKey r = resolveApiKey(c);
	CHECK_EQ(r.key, "file-key");
	CHECK_EQ(r.source, "assistant.json");

	setenv("OPENROUTER_API_KEY", "or-key", 1);
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "or-key");
	CHECK_EQ(r.source, "OPENROUTER_API_KEY");

	setenv("RACK_ASSISTANT_API_KEY", "ra-key", 1);
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "ra-key");
	CHECK_EQ(r.source, "RACK_ASSISTANT_API_KEY");

	// Empty env vars are skipped
	setenv("RACK_ASSISTANT_API_KEY", "", 1);
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "or-key");
	CHECK_EQ(r.source, "OPENROUTER_API_KEY");
	setenv("OPENROUTER_API_KEY", "", 1);
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "file-key");
	CHECK_EQ(r.source, "assistant.json");

	// Nothing at all
	c.apiKey = "";
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "");
	CHECK_EQ(r.source, "");
}

TEST(key_is_trimmed) {
	EnvGuard g1("RACK_ASSISTANT_API_KEY");
	EnvGuard g2("OPENROUTER_API_KEY");
	Config c;
	unsetenv("RACK_ASSISTANT_API_KEY");
	unsetenv("OPENROUTER_API_KEY");

	// Trailing newline / CRLF / spaces from echo, cat or pasting
	setenv("RACK_ASSISTANT_API_KEY", "ra-key\n", 1);
	CHECK_EQ(resolveApiKey(c).key, "ra-key");
	setenv("RACK_ASSISTANT_API_KEY", "  ra-key\r\n", 1);
	CHECK_EQ(resolveApiKey(c).key, "ra-key");
	setenv("RACK_ASSISTANT_API_KEY", "\t ra-key \t", 1);
	CHECK_EQ(resolveApiKey(c).key, "ra-key");

	// A whitespace-only env var counts as empty and is skipped
	setenv("RACK_ASSISTANT_API_KEY", " \n", 1);
	setenv("OPENROUTER_API_KEY", "or-key\n", 1);
	ResolvedKey r = resolveApiKey(c);
	CHECK_EQ(r.key, "or-key");
	CHECK_EQ(r.source, "OPENROUTER_API_KEY");

	// Same for the stored key
	unsetenv("RACK_ASSISTANT_API_KEY");
	unsetenv("OPENROUTER_API_KEY");
	c.apiKey = "  file-key\n";
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "file-key");
	CHECK_EQ(r.source, "assistant.json");
	c.apiKey = " \r\n";
	r = resolveApiKey(c);
	CHECK_EQ(r.key, "");
	CHECK_EQ(r.source, "");
}

TEST(temperature_is_written_as_decimal) {
	Config c;
	c.hasTemperature = true;
	c.temperature = 0.7f;
	CHECK_EQ(temperatureToDouble(0.7f), 0.7);
	CHECK_EQ(temperatureToDouble(1.f), 1.0);
	CHECK_EQ(temperatureToDouble(0.15f), 0.15);
	json_t* j = configToJson(c);
	REQUIRE(j != NULL);
	json_t* t = json_object_get(j, "temperature");
	REQUIRE(json_is_real(t));
	CHECK_EQ(json_real_value(t), 0.7);
	json_decref(j);

	// The saved file shows what the user typed
	std::string dir = makeTempDir("temperature");
	std::string path = system::join(dir, "assistant.json");
	std::string error;
	REQUIRE(saveConfig(c, path, &error));
	std::string text = readFile(path);
	CHECK(text.find("\"temperature\": 0.7") != std::string::npos);
	CHECK(text.find("0.699") == std::string::npos);
	system::removeRecursively(dir);
}

TEST(key_mask) {
	CHECK_EQ(maskKey("sk-or-v1-0000111122223333abcd"), "sk-or-v1\xE2\x80\xA6" "abcd");
	CHECK_EQ(maskKey(""), "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
	CHECK_EQ(maskKey("short"), "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
	CHECK_EQ(maskKey("123456789012345"), "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
	// 16 chars is the shortest masked-by-prefix key
	CHECK_EQ(maskKey("1234567890123456"), "12345678\xE2\x80\xA6" "3456");
	// the middle of a key never appears
	std::string m = maskKey("sk-or-v1-SECRETSECRETSECRETSECRETxxxx");
	CHECK(m.find("SECRET") == std::string::npos);
}

TEST(validators) {
	CHECK(isValidReasoningEffort("off"));
	CHECK(isValidReasoningEffort("none"));
	CHECK(isValidReasoningEffort("minimal"));
	CHECK(isValidReasoningEffort("low"));
	CHECK(isValidReasoningEffort("medium"));
	CHECK(isValidReasoningEffort("high"));
	CHECK(!isValidReasoningEffort(""));
	CHECK(!isValidReasoningEffort("High"));
	CHECK(!isValidReasoningEffort("xhigh"));
	CHECK(isValidReasoningParamStyle("openrouter"));
	CHECK(isValidReasoningParamStyle("openai"));
	CHECK(!isValidReasoningParamStyle(""));
	CHECK(!isValidReasoningParamStyle("OpenAI"));
}

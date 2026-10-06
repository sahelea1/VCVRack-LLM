#include "test.hpp"

#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>
#include <asset.hpp>
#include <system.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>


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


std::shared_ptr<LlmClient> makeMock(const std::string& scriptPath = "") {
	ClientOptions o;
	o.config.mock = true;
	o.mockScriptPath = scriptPath;
	return createClient(o);
}


struct Conversation {
	std::shared_ptr<LlmClient> client;
	std::vector<ChatMessage> messages;
	std::atomic<bool> cancel;

	explicit Conversation(const std::string& userText, std::shared_ptr<LlmClient> c = makeMock()) : client(c), cancel(false) {
		ChatMessage sys;
		sys.role = ChatMessage::SYSTEM;
		sys.content = "system";
		messages.push_back(sys);
		ChatMessage user;
		user.role = ChatMessage::USER;
		user.content = userText;
		messages.push_back(user);
	}

	ChatResponse next() {
		ChatRequest req;
		req.messages = messages;
		req.toolsJson = "[]";
		ChatResponse r = client->complete(req, cancel);
		if (r.ok)
			messages.push_back(r.message);
		return r;
	}

	void addToolResult(const ToolCall& tc, const std::string& content) {
		ChatMessage t;
		t.role = ChatMessage::TOOL;
		t.toolCallId = tc.id;
		t.content = content;
		messages.push_back(t);
	}
};


std::string argString(const ToolCall& tc, const char* key) {
	JsonPtr j(parse(tc.arguments));
	json_t* v = j.j ? json_object_get(j.j, key) : NULL;
	if (v && json_is_string(v))
		return json_string_value(v);
	return "<missing>";
}


long long argInt(const ToolCall& tc, const char* key) {
	JsonPtr j(parse(tc.arguments));
	json_t* v = j.j ? json_object_get(j.j, key) : NULL;
	if (v && json_is_integer(v))
		return json_integer_value(v);
	return -999999;
}


double argReal(const ToolCall& tc, const char* key) {
	JsonPtr j(parse(tc.arguments));
	json_t* v = j.j ? json_object_get(j.j, key) : NULL;
	if (v && json_is_number(v))
		return json_number_value(v);
	return -999999.0;
}


const char* VCO_INFO = "{\"ok\":true,\"plugin\":\"Fundamental\",\"model\":\"VCO\",\"module_id\":101,"
	"\"params\":[{\"id\":0,\"name\":\"Mode\"}],"
	"\"inputs\":[{\"id\":0,\"name\":\"V/Oct\"}],"
	"\"outputs\":[{\"id\":0,\"name\":\"Sine\"},{\"id\":1,\"name\":\"Triangle\"},{\"id\":2,\"name\":\"Saw\"},{\"id\":3,\"name\":\"Square\"}]}";

const char* VCF_INFO = "{\"ok\":true,\"plugin\":\"Fundamental\",\"model\":\"VCF\",\"module_id\":102,"
	"\"params\":[{\"id\":0,\"name\":\"Drive\"},{\"id\":1,\"name\":\"Resonance\"},{\"id\":2,\"name\":\"Frequency\"}],"
	"\"inputs\":[{\"id\":0,\"name\":\"Frequency\"},{\"id\":1,\"name\":\"Resonance\"},{\"id\":2,\"name\":\"Drive\"},{\"id\":3,\"name\":\"Audio\"}],"
	"\"outputs\":[{\"id\":0,\"name\":\"Lowpass\"},{\"id\":1,\"name\":\"Highpass\"}]}";

} // namespace


TEST(mock_plain_reply) {
	Conversation c("Hello rack");
	ChatResponse r = c.next();
	REQUIRE(r.ok);
	CHECK_EQ(r.message.content, "Mock reply: Hello rack");
	CHECK(r.message.toolCalls.empty());
	CHECK_EQ(r.finishReason, "stop");
	CHECK_EQ(r.model, "mock");
}

TEST(mock_plain_reply_truncated_to_200_chars) {
	std::string long200;
	for (int i = 0; i < 300; i++)
		long200 += "x";
	Conversation c(long200);
	ChatResponse r = c.next();
	REQUIRE(r.ok);
	CHECK_EQ((int) r.message.content.size(), (int) std::string("Mock reply: ").size() + 200);
	// multi-byte characters are cut on code point boundaries
	std::string umlauts;
	for (int i = 0; i < 300; i++)
		umlauts += "\xC3\xA4";
	Conversation d(umlauts);
	ChatResponse s = d.next();
	REQUIRE(s.ok);
	CHECK_EQ((int) s.message.content.size(), (int) std::string("Mock reply: ").size() + 400);
}

TEST(mock_uses_last_user_message) {
	Conversation c("first question");
	ChatResponse r = c.next();
	REQUIRE(r.ok);
	ChatMessage u;
	u.role = ChatMessage::USER;
	u.content = "second question";
	c.messages.push_back(u);
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK_EQ(r2.message.content, "Mock reply: second question");
}

TEST(mock_demo_build_scenario) {
	// Any of these keywords triggers the demo build
	const char* triggers[] = {"/mock-build", "please build something", "Bau mir was", "a demo", "make it ACID"};
	for (const char* trigger : triggers) {
		Conversation c(trigger);

		// r0: get_patch + search_modules, content null
		ChatResponse r0 = c.next();
		REQUIRE(r0.ok);
		CHECK(r0.message.contentNull);
		CHECK_EQ(r0.finishReason, "tool_calls");
		REQUIRE(r0.message.toolCalls.size() == 2);
		CHECK_EQ(r0.message.toolCalls[0].name, "get_patch");
		CHECK_EQ(r0.message.toolCalls[0].id, "mock_0_0");
		CHECK_EQ(r0.message.toolCalls[1].name, "search_modules");
		CHECK_EQ(r0.message.toolCalls[1].id, "mock_0_1");
		CHECK_EQ(argString(r0.message.toolCalls[1], "query"), "VCO");
		c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true,\"module_count\":0,\"modules\":[],\"cables\":[]}");
		c.addToolResult(r0.message.toolCalls[1], "{\"ok\":true,\"total_matches\":1,\"results\":[{\"plugin\":\"Fundamental\",\"model\":\"VCO\"}]}");

		// r1: two add_module calls
		ChatResponse r1 = c.next();
		REQUIRE(r1.ok);
		REQUIRE(r1.message.toolCalls.size() == 2);
		CHECK_EQ(r1.message.toolCalls[0].name, "add_module");
		CHECK_EQ(argString(r1.message.toolCalls[0], "plugin"), "Fundamental");
		CHECK_EQ(argString(r1.message.toolCalls[0], "model"), "VCO");
		CHECK_EQ(r1.message.toolCalls[1].name, "add_module");
		CHECK_EQ(argString(r1.message.toolCalls[1], "model"), "VCF");
		CHECK_EQ(r1.message.toolCalls[1].id, "mock_1_1");
		c.addToolResult(r1.message.toolCalls[0], "{\"ok\":true,\"module_id\":101,\"plugin\":\"Fundamental\",\"model\":\"VCO\"}");
		c.addToolResult(r1.message.toolCalls[1], "{\"ok\":true,\"module_id\":102,\"plugin\":\"Fundamental\",\"model\":\"VCF\"}");

		// r2: get_module_info for both new ids
		ChatResponse r2 = c.next();
		REQUIRE(r2.ok);
		REQUIRE(r2.message.toolCalls.size() == 2);
		CHECK_EQ(r2.message.toolCalls[0].name, "get_module_info");
		CHECK_EQ(argInt(r2.message.toolCalls[0], "module_id"), 101);
		CHECK_EQ(r2.message.toolCalls[1].name, "get_module_info");
		CHECK_EQ(argInt(r2.message.toolCalls[1], "module_id"), 102);
		c.addToolResult(r2.message.toolCalls[0], VCO_INFO);
		c.addToolResult(r2.message.toolCalls[1], VCF_INFO);

		// r3: connect saw -> audio, set cutoff
		ChatResponse r3 = c.next();
		REQUIRE(r3.ok);
		REQUIRE(r3.message.toolCalls.size() == 2);
		const ToolCall& connect = r3.message.toolCalls[0];
		CHECK_EQ(connect.name, "connect");
		CHECK_EQ(argInt(connect, "from_module"), 101);
		CHECK_EQ(argInt(connect, "output_id"), 2);
		CHECK_EQ(argInt(connect, "to_module"), 102);
		CHECK_EQ(argInt(connect, "input_id"), 3);
		const ToolCall& setParam = r3.message.toolCalls[1];
		CHECK_EQ(setParam.name, "set_param");
		CHECK_EQ(argInt(setParam, "module_id"), 102);
		CHECK_EQ(argInt(setParam, "param_id"), 2);
		CHECK(argReal(setParam, "value") > 0.399 && argReal(setParam, "value") < 0.401);
		c.addToolResult(connect, "{\"ok\":true,\"cable_id\":5}");
		c.addToolResult(setParam, "{\"ok\":true}");

		// r4: final text
		ChatResponse r4 = c.next();
		REQUIRE(r4.ok);
		CHECK(r4.message.toolCalls.empty());
		CHECK_EQ(r4.message.content, "Mock: built VCO \xE2\x86\x92 VCF. Try turning Cutoff and Resonance.");
		CHECK_EQ(r4.finishReason, "stop");
	}
}

TEST(mock_demo_build_port_fallbacks) {
	// No "saw" output / no audio-ish input / no cutoff param: falls back to index 0
	Conversation c("/mock-build");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true}");
	c.addToolResult(r0.message.toolCalls[1], "{\"ok\":true}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	c.addToolResult(r1.message.toolCalls[0], "{\"ok\":true,\"module_id\":\"7\"}");  // numeric string id
	c.addToolResult(r1.message.toolCalls[1], "{\"ok\":true,\"module_id\":8}");
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK_EQ(argInt(r2.message.toolCalls[0], "module_id"), 7);
	c.addToolResult(r2.message.toolCalls[0], "{\"ok\":true,\"module_id\":7,\"outputs\":[{\"id\":5,\"name\":\"Foo\"}]}");
	c.addToolResult(r2.message.toolCalls[1], "{\"ok\":true,\"module_id\":8,\"inputs\":[{\"id\":4,\"name\":\"Gate\"},{\"id\":9,\"name\":\"Pitch In\"}],\"params\":[{\"id\":3,\"name\":\"Mix\"}]}");
	ChatResponse r3 = c.next();
	REQUIRE(r3.ok);
	REQUIRE(r3.message.toolCalls.size() == 2);
	CHECK_EQ(argInt(r3.message.toolCalls[0], "output_id"), 0);
	CHECK_EQ(argInt(r3.message.toolCalls[0], "input_id"), 9);   // contains "in"
	CHECK_EQ(argInt(r3.message.toolCalls[1], "param_id"), 0);
}

TEST(mock_demo_build_failure_texts) {
	// add_module failed: round 2 explains instead of calling tools
	Conversation c("/mock-build");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true}");
	c.addToolResult(r0.message.toolCalls[1], "{\"ok\":true}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	c.addToolResult(r1.message.toolCalls[0], "{\"ok\":false,\"error\":\"Unknown model Fundamental/VCO\"}");
	c.addToolResult(r1.message.toolCalls[1], "{\"ok\":true,\"module_id\":8}");
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK(r2.message.toolCalls.empty());
	CHECK(r2.message.content.find("Unknown model Fundamental/VCO") != std::string::npos);

	// tool results missing / not JSON
	Conversation d("/mock-build");
	ChatResponse a0 = d.next();
	REQUIRE(a0.ok);
	d.addToolResult(a0.message.toolCalls[0], "x");
	d.addToolResult(a0.message.toolCalls[1], "x");
	ChatResponse a1 = d.next();
	REQUIRE(a1.ok);
	d.addToolResult(a1.message.toolCalls[0], "garbage not json");
	d.addToolResult(a1.message.toolCalls[1], "[1,2,3]");
	ChatResponse a2 = d.next();
	REQUIRE(a2.ok);
	CHECK(a2.message.toolCalls.empty());
	CHECK(!a2.message.content.empty());
}

TEST(mock_error_scenarios) {
	struct Row {
		const char* text;
		LlmError::Kind kind;
		int status;
	};
	const Row rows[] = {
		{"/mock-error 402", LlmError::PAYMENT, 402},
		{"please /mock-error 429 now", LlmError::RATE_LIMIT, 429},
		{"/mock-error 500", LlmError::SERVER, 500},
		{"/mock-error 401", LlmError::AUTH, 401},
		{"/mock-error 400", LlmError::BAD_REQUEST, 400},
		{"/MOCK-ERROR 404", LlmError::NOT_FOUND, 404},
		{"/mock-error", LlmError::SERVER, 500},   // no code: defaults to 500
	};
	for (const Row& row : rows) {
		Conversation c(row.text);
		ChatResponse r = c.next();
		CHECK(!r.ok);
		CHECK_EQ((int) r.error.kind, (int) row.kind);
		CHECK_EQ(r.error.httpStatus, row.status);
		CHECK_EQ(r.error.detail, "mock error");
	}
}

TEST(mock_badargs_scenario) {
	Conversation c("/mock-badargs");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	REQUIRE(r0.message.toolCalls.size() == 1);
	CHECK_EQ(r0.message.toolCalls[0].name, "add_module");
	CHECK_EQ(r0.message.toolCalls[0].arguments, "{not json");
	CHECK_EQ(r0.message.toolCalls[0].id, "mock_0_0");
	CHECK(r0.message.contentNull);
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":false,\"error\":\"Invalid arguments\"}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	CHECK(r1.message.toolCalls.empty());
	CHECK_EQ(r1.message.content, "The tool reported invalid arguments, as expected.");
}

TEST(mock_delete_scenario) {
	Conversation c("/mock-delete");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	REQUIRE(r0.message.toolCalls.size() == 1);
	CHECK_EQ(r0.message.toolCalls[0].name, "get_patch");
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true,\"module_count\":2,\"modules\":[{\"id\":4242,\"plugin\":\"Fundamental\",\"model\":\"VCO\"},{\"id\":7,\"plugin\":\"Fundamental\",\"model\":\"VCF\"}],\"cables\":[]}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	REQUIRE(r1.message.toolCalls.size() == 1);
	CHECK_EQ(r1.message.toolCalls[0].name, "remove_module");
	CHECK_EQ(argInt(r1.message.toolCalls[0], "module_id"), 4242);
	c.addToolResult(r1.message.toolCalls[0], "{\"ok\":true,\"removed_module_id\":4242,\"removed_cables\":0}");
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK(r2.message.toolCalls.empty());
	CHECK(r2.message.content.find("4242") != std::string::npos);
}

TEST(mock_delete_declined_and_empty_patch) {
	Conversation c("/mock-delete");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true,\"modules\":[{\"id\":5}]}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	c.addToolResult(r1.message.toolCalls[0], "{\"ok\":false,\"error\":\"The user declined this action.\"}");
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK(r2.message.toolCalls.empty());
	CHECK(r2.message.content.find("not removed") != std::string::npos);
	CHECK(r2.message.content.find("declined") != std::string::npos);

	Conversation d("/mock-delete");
	ChatResponse a0 = d.next();
	REQUIRE(a0.ok);
	d.addToolResult(a0.message.toolCalls[0], "{\"ok\":true,\"modules\":[]}");
	ChatResponse a1 = d.next();
	REQUIRE(a1.ok);
	CHECK(a1.message.toolCalls.empty());
	CHECK(!a1.message.content.empty());
}

TEST(mock_loop_scenario) {
	Conversation c("/mock-loop");
	for (int round = 0; round < 6; round++) {
		ChatResponse r = c.next();
		REQUIRE(r.ok);
		REQUIRE(r.message.toolCalls.size() == 1);
		CHECK_EQ(r.message.toolCalls[0].name, "get_patch");
		CHECK_EQ(r.message.toolCalls[0].id, "mock_" + std::to_string(round) + "_0");
		c.addToolResult(r.message.toolCalls[0], "{\"ok\":true,\"modules\":[]}");
	}
}

TEST(mock_round_counts_only_after_last_user) {
	// A second /mock-badargs turn starts again at round 0
	Conversation c("/mock-badargs");
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":false}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	ChatMessage u;
	u.role = ChatMessage::USER;
	u.content = "/mock-badargs again";
	c.messages.push_back(u);
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	REQUIRE(r2.message.toolCalls.size() == 1);
	CHECK_EQ(r2.message.toolCalls[0].id, "mock_0_0");
}

TEST(mock_cancel_before_start) {
	Conversation c("hello");
	c.cancel = true;
	auto t0 = std::chrono::steady_clock::now();
	ChatResponse r = c.next();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CANCELLED);
	CHECK_EQ(r.error.message, "Cancelled.");
	CHECK(ms < 100.0);
}

TEST(mock_cancel_during_latency) {
	Conversation c("hello");
	std::thread canceller([&c]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		c.cancel = true;
	});
	auto t0 = std::chrono::steady_clock::now();
	ChatResponse r = c.next();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	canceller.join();
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CANCELLED);
	// cancelled at ~60 ms, well before the 250 ms simulated latency is over
	CHECK(ms < 200.0);
	CHECK(ms >= 40.0);
}

TEST(mock_simulated_latency) {
	Conversation c("hello");
	auto t0 = std::chrono::steady_clock::now();
	ChatResponse r = c.next();
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	CHECK(r.ok);
	CHECK(ms >= 200.0);
	CHECK(ms < 1000.0);
}

TEST(mock_script_file) {
	std::string dir = system::join(asset::user(""), "mock-script-test");
	system::removeRecursively(dir);
	system::createDirectories(dir);
	std::string path = system::join(dir, "assistant-mock.json");
	{
		std::ofstream f(path.c_str());
		f << "{\"responses\":["
			"{\"model\":\"scripted\",\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"s1\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"{}\"}}]}}]},"
			"{\"model\":\"scripted\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"scripted answer\"}}]}"
			"]}";
	}
	Conversation c("anything, even /mock-error 500", makeMock(path));
	ChatResponse r0 = c.next();
	REQUIRE(r0.ok);
	CHECK_EQ(r0.model, "scripted");
	REQUIRE(r0.message.toolCalls.size() == 1);
	CHECK_EQ(r0.message.toolCalls[0].id, "s1");
	c.addToolResult(r0.message.toolCalls[0], "{\"ok\":true}");
	ChatResponse r1 = c.next();
	REQUIRE(r1.ok);
	CHECK_EQ(r1.message.content, "scripted answer");
	// Index is clamped to the last response
	ChatMessage a;
	a.role = ChatMessage::ASSISTANT;
	a.content = "x";
	c.messages.push_back(a);
	c.messages.push_back(a);
	ChatResponse r2 = c.next();
	REQUIRE(r2.ok);
	CHECK_EQ(r2.message.content, "scripted answer");

	// An invalid script falls back to the built-in scenarios
	std::string badPath = system::join(dir, "bad.json");
	{
		std::ofstream f(badPath.c_str());
		f << "{\"responses\": 5}";
	}
	Conversation d("hello", makeMock(badPath));
	ChatResponse r3 = d.next();
	REQUIRE(r3.ok);
	CHECK_EQ(r3.message.content, "Mock reply: hello");

	// A missing script file too
	Conversation e("hello", makeMock(system::join(dir, "nope.json")));
	ChatResponse r4 = e.next();
	REQUIRE(r4.ok);
	CHECK_EQ(r4.message.content, "Mock reply: hello");
	system::removeRecursively(dir);
}

TEST(client_factory_selection) {
	// mock flag
	ClientOptions a;
	a.config.mock = true;
	a.mockScriptPath = "";
	std::atomic<bool> cancel(false);
	ChatRequest req;
	ChatMessage u;
	u.role = ChatMessage::USER;
	u.content = "hi";
	req.messages.push_back(u);
	ChatResponse r = createClient(a)->complete(req, cancel);
	CHECK(r.ok);
	CHECK_EQ(r.model, "mock");

	// mock: base url prefix
	ClientOptions b;
	b.config.baseUrl = "mock://anything";
	b.mockScriptPath = "";
	ChatResponse s = createClient(b)->complete(req, cancel);
	CHECK(s.ok);
	CHECK_EQ(s.model, "mock");

	// HTTP client without key and a remote host: config error, no network involved
	ClientOptions c;
	c.config.baseUrl = "https://openrouter.ai/api/v1";
	c.apiKey = "";
	ChatResponse t = createClient(c)->complete(req, cancel);
	CHECK(!t.ok);
	CHECK_EQ((int) t.error.kind, (int) LlmError::CONFIG);
	CHECK(t.error.message.find("No API key configured") != std::string::npos);
}

TEST(client_options_resolution) {
	Config cfg;
	cfg.caBundle = "/some/ca.pem";
	ClientOptions o = makeClientOptions(cfg);
	CHECK_EQ(o.caBundlePath, "/some/ca.pem");
	CHECK(o.userAgent.find("VCV Rack ") == 0);
	CHECK(o.userAgent.find("Assistant") != std::string::npos);
	CHECK(o.mockScriptPath.find("assistant-mock.json") != std::string::npos);
	CHECK(o.verifyTls);

	Config plain;
	const char* old = getenv("RACK_ASSISTANT_CA_BUNDLE");
	std::string oldValue = old ? old : "";
	setenv("RACK_ASSISTANT_CA_BUNDLE", "/env/ca.pem", 1);
	CHECK_EQ(makeClientOptions(plain).caBundlePath, "/env/ca.pem");
	unsetenv("RACK_ASSISTANT_CA_BUNDLE");
	CHECK(makeClientOptions(plain).caBundlePath.find("cacert.pem") != std::string::npos);
	if (old)
		setenv("RACK_ASSISTANT_CA_BUNDLE", oldValue.c_str(), 1);
}


TEST(mock_base_url_detection_is_trimmed_and_case_insensitive) {
	const char* urls[] = {"mock:", " mock:", "Mock:x", "\tMOCK:\n"};
	for (const char* url : urls) {
		ClientOptions o;
		o.config.baseUrl = url;
		o.config.mock = false;
		o.mockScriptPath = "";
		std::shared_ptr<LlmClient> client = createClient(o);
		Conversation conv("hello", client);
		ChatResponse r = conv.next();
		REQUIRE(r.ok);
		CHECK_EQ(r.message.content, "Mock reply: hello");
	}
}

// Optional live tests against a real provider. Only run with `assistant_test --live`.
//
// Environment:
//   RACK_ASSISTANT_API_KEY      key (required)
//   RACK_ASSISTANT_BASE_URL     default https://openrouter.ai/api/v1
//   RACK_ASSISTANT_MODEL        default openai/gpt-5.6-terra
//   RACK_ASSISTANT_PARAM_STYLE  default openrouter
//   RACK_ASSISTANT_REASONING_EFFORT  default low (off|minimal|low|medium|high)
//   RACK_ASSISTANT_CA_BUNDLE    optional CA file (needed behind a TLS-intercepting proxy)
//
// Output is limited to the model name, finish_reason, token usage and pass/fail. Request
// bodies and the key are never printed.
#include "test.hpp"

#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>

#include <atomic>
#include <cstdlib>


using namespace rack;
using namespace rack::assistant;


namespace {

std::string envOr(const char* name, const std::string& fallback) {
	const char* v = std::getenv(name);
	return (v && v[0]) ? v : fallback;
}


struct LiveSetup {
	ClientOptions options;
	bool ok = false;
};


LiveSetup makeSetup() {
	LiveSetup s;
	const char* key = std::getenv("RACK_ASSISTANT_API_KEY");
	if (!key || !key[0]) {
		test::fail(__FILE__, __LINE__, "RACK_ASSISTANT_API_KEY is not set");
		return s;
	}
	ClientOptions& o = s.options;
	o.config.baseUrl = envOr("RACK_ASSISTANT_BASE_URL", "https://openrouter.ai/api/v1");
	o.config.model = envOr("RACK_ASSISTANT_MODEL", "openai/gpt-5.6-terra");
	o.config.reasoningParamStyle = envOr("RACK_ASSISTANT_PARAM_STYLE", "openrouter");
	o.config.reasoningEffort = envOr("RACK_ASSISTANT_REASONING_EFFORT", "low");
	o.config.maxTokens = 0;
	o.config.timeoutSec = 120.0;
	o.apiKey = key;
	o.verifyTls = true;
	o.caBundlePath = envOr("RACK_ASSISTANT_CA_BUNDLE", "");
	o.userAgent = "VCV Rack live-test Assistant";
	s.ok = true;
	return s;
}


void report(const char* label, const ChatResponse& r) {
	if (r.ok) {
		std::printf("    [live] %s: model=%s finish_reason=%s tokens prompt=%d completion=%d total=%d reasoning=%d\n",
			label, r.model.c_str(), r.finishReason.c_str(), r.usage.promptTokens, r.usage.completionTokens,
			r.usage.totalTokens, r.usage.reasoningTokens);
	}
	else {
		std::printf("    [live] %s: ERROR kind=%d status=%d message=%s | detail=%s\n",
			label, (int) r.error.kind, r.error.httpStatus, r.error.message.c_str(), r.error.detail.c_str());
	}
	std::fflush(stdout);
}


ChatMessage msg(ChatMessage::Role role, const std::string& content) {
	ChatMessage m;
	m.role = role;
	m.content = content;
	return m;
}

} // namespace


TEST_LIVE(live_text_request) {
	LiveSetup s = makeSetup();
	REQUIRE(s.ok);
	std::shared_ptr<LlmClient> client = createHttpClient(s.options);
	ChatRequest req;
	req.messages.push_back(msg(ChatMessage::SYSTEM, "You are a terse assistant."));
	req.messages.push_back(msg(ChatMessage::USER, "Reply with the single word: pong"));
	std::atomic<bool> cancel(false);
	ChatResponse r = client->complete(req, cancel);
	report("text", r);
	REQUIRE(r.ok);
	CHECK(!r.message.content.empty());
	CHECK(r.message.toolCalls.empty());
	CHECK(!r.finishReason.empty());
	CHECK(r.usage.totalTokens > 0);
	std::printf("    [live] text: %s\n", (r.ok && !r.message.content.empty()) ? "PASS" : "FAIL");
}


TEST_LIVE(live_tool_call_roundtrip) {
	LiveSetup s = makeSetup();
	REQUIRE(s.ok);
	std::shared_ptr<LlmClient> client = createHttpClient(s.options);
	ChatRequest req;
	req.toolsJson = "[{\"type\":\"function\",\"function\":{\"name\":\"get_time\","
		"\"description\":\"Returns the current time of day.\","
		"\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]";
	req.messages.push_back(msg(ChatMessage::SYSTEM, "You are a helpful assistant. Use the provided tools when they are needed."));
	req.messages.push_back(msg(ChatMessage::USER, "What time is it right now? Call the get_time tool to find out."));
	std::atomic<bool> cancel(false);

	ChatResponse r1 = client->complete(req, cancel);
	report("tool-request", r1);
	REQUIRE(r1.ok);
	REQUIRE(!r1.message.toolCalls.empty());
	CHECK_EQ(r1.message.toolCalls[0].name, "get_time");
	CHECK(!r1.message.toolCalls[0].id.empty());
	std::printf("    [live] tool-request: %s\n", (!r1.message.toolCalls.empty() && r1.message.toolCalls[0].name == "get_time") ? "PASS" : "FAIL");

	// Follow-up with the tool result (also exercises echoing reasoning_details back)
	req.messages.push_back(r1.message);
	for (const ToolCall& tc : r1.message.toolCalls) {
		ChatMessage t;
		t.role = ChatMessage::TOOL;
		t.toolCallId = tc.id;
		t.content = "{\"ok\":true,\"time\":\"13:37\"}";
		req.messages.push_back(t);
	}
	ChatResponse r2 = client->complete(req, cancel);
	report("tool-followup", r2);
	REQUIRE(r2.ok);
	CHECK(!r2.message.content.empty());
	CHECK(r2.message.toolCalls.empty());
	std::printf("    [live] tool-followup: %s\n", (r2.ok && !r2.message.content.empty()) ? "PASS" : "FAIL");
}

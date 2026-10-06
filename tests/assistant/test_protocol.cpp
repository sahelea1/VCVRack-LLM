#include "test.hpp"

#include <assistant/Protocol.hpp>

#include <climits>
#include <memory>


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
	json_t* operator->() const { return j; }
};


json_t* parse(const std::string& s) {
	json_error_t error;
	return json_loads(s.c_str(), 0, &error);
}


std::string str(json_t* j, const char* key) {
	json_t* v = json_object_get(j, key);
	return (v && json_is_string(v)) ? json_string_value(v) : "<missing>";
}


ChatRequest simpleRequest() {
	ChatRequest req;
	ChatMessage sys;
	sys.role = ChatMessage::SYSTEM;
	sys.content = "You are a test.";
	ChatMessage user;
	user.role = ChatMessage::USER;
	user.content = "Hello";
	req.messages.push_back(sys);
	req.messages.push_back(user);
	return req;
}

const char* TOOLS = "[{\"type\":\"function\",\"function\":{\"name\":\"get_patch\",\"description\":\"d\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]";

} // namespace


// URL

TEST(url_basic) {
	CHECK_EQ(chatCompletionsUrl("https://openrouter.ai/api/v1"), "https://openrouter.ai/api/v1/chat/completions");
	CHECK_EQ(chatCompletionsUrl("https://openrouter.ai/api/v1/"), "https://openrouter.ai/api/v1/chat/completions");
	CHECK_EQ(chatCompletionsUrl("https://openrouter.ai/api/v1///"), "https://openrouter.ai/api/v1/chat/completions");
	CHECK_EQ(chatCompletionsUrl("https://x/v1/chat/completions"), "https://x/v1/chat/completions");
	CHECK_EQ(chatCompletionsUrl("https://x/v1/chat/completions/"), "https://x/v1/chat/completions");
	CHECK_EQ(chatCompletionsUrl("  http://localhost:11434/v1  "), "http://localhost:11434/v1/chat/completions");
}


// Request body

TEST(body_openrouter_defaults) {
	Config c;
	std::string body = buildRequestBody(c, simpleRequest());
	JsonPtr j(parse(body));
	REQUIRE(j.j);
	CHECK_EQ(str(j.j, "model"), "openai/gpt-5.6-terra");
	json_t* messages = json_object_get(j.j, "messages");
	REQUIRE(json_is_array(messages));
	CHECK_EQ((int) json_array_size(messages), 2);
	CHECK_EQ(str(json_array_get(messages, 0), "role"), "system");
	CHECK_EQ(str(json_array_get(messages, 1), "content"), "Hello");
	// Reasoning, openrouter style
	json_t* reasoning = json_object_get(j.j, "reasoning");
	REQUIRE(json_is_object(reasoning));
	CHECK_EQ(str(reasoning, "effort"), "medium");
	CHECK(!json_object_get(j.j, "reasoning_effort"));
	// Unset parameters are not sent
	CHECK(!json_object_get(j.j, "max_tokens"));
	CHECK(!json_object_get(j.j, "max_completion_tokens"));
	CHECK(!json_object_get(j.j, "temperature"));
	CHECK(!json_object_get(j.j, "tools"));
	CHECK(!json_object_get(j.j, "tool_choice"));
	// Compact
	CHECK(body.find('\n') == std::string::npos);
	CHECK(body.find(": ") == std::string::npos);
}

TEST(body_openai_style) {
	Config c;
	c.reasoningParamStyle = "openai";
	c.reasoningEffort = "low";
	c.maxTokens = 777;
	JsonPtr j(parse(buildRequestBody(c, simpleRequest())));
	REQUIRE(j.j);
	CHECK_EQ(str(j.j, "reasoning_effort"), "low");
	CHECK(!json_object_get(j.j, "reasoning"));
	CHECK(!json_object_get(j.j, "max_tokens"));
	json_t* mct = json_object_get(j.j, "max_completion_tokens");
	REQUIRE(json_is_integer(mct));
	CHECK_EQ((int) json_integer_value(mct), 777);
}

TEST(body_max_tokens_openrouter) {
	Config c;
	c.maxTokens = 1234;
	JsonPtr j(parse(buildRequestBody(c, simpleRequest())));
	REQUIRE(j.j);
	json_t* mt = json_object_get(j.j, "max_tokens");
	REQUIRE(json_is_integer(mt));
	CHECK_EQ((int) json_integer_value(mt), 1234);
	CHECK(!json_object_get(j.j, "max_completion_tokens"));
}

TEST(body_reasoning_off) {
	Config c;
	c.reasoningEffort = "off";
	JsonPtr a(parse(buildRequestBody(c, simpleRequest())));
	CHECK(!json_object_get(a.j, "reasoning"));
	CHECK(!json_object_get(a.j, "reasoning_effort"));
	c.reasoningParamStyle = "openai";
	JsonPtr b(parse(buildRequestBody(c, simpleRequest())));
	CHECK(!json_object_get(b.j, "reasoning"));
	CHECK(!json_object_get(b.j, "reasoning_effort"));
}

TEST(body_reasoning_none_is_sent) {
	// "none" differs from "off": the parameter is sent with the value "none"
	Config c;
	c.reasoningEffort = "none";
	JsonPtr a(parse(buildRequestBody(c, simpleRequest())));
	REQUIRE(json_is_object(json_object_get(a.j, "reasoning")));
	CHECK_EQ(str(json_object_get(a.j, "reasoning"), "effort"), "none");
	c.reasoningParamStyle = "openai";
	JsonPtr b(parse(buildRequestBody(c, simpleRequest())));
	CHECK_EQ(str(b.j, "reasoning_effort"), "none");
}

TEST(body_temperature) {
	Config c;
	c.hasTemperature = true;
	c.temperature = 0.5f;
	JsonPtr j(parse(buildRequestBody(c, simpleRequest())));
	json_t* t = json_object_get(j.j, "temperature");
	REQUIRE(json_is_real(t));
	CHECK(json_real_value(t) > 0.49 && json_real_value(t) < 0.51);
}

TEST(body_temperature_is_decimal_not_float_noise) {
	Config c;
	c.hasTemperature = true;
	c.temperature = 0.7f;
	std::string body = buildRequestBody(c, simpleRequest());
	CHECK(body.find("\"temperature\":0.7,") != std::string::npos || body.find("\"temperature\":0.7}") != std::string::npos);
	CHECK(body.find("0.699") == std::string::npos);
	c.temperature = 1.f;
	CHECK(buildRequestBody(c, simpleRequest()).find("\"temperature\":1.0") != std::string::npos);
	c.temperature = 0.15f;
	CHECK(buildRequestBody(c, simpleRequest()).find("\"temperature\":0.15") != std::string::npos);
}

TEST(body_tools) {
	Config c;
	ChatRequest req = simpleRequest();
	req.toolsJson = TOOLS;
	JsonPtr j(parse(buildRequestBody(c, req)));
	REQUIRE(j.j);
	json_t* tools = json_object_get(j.j, "tools");
	REQUIRE(json_is_array(tools));
	CHECK_EQ((int) json_array_size(tools), 1);
	CHECK_EQ(str(j.j, "tool_choice"), "auto");
	// Invalid tools JSON is ignored instead of producing a broken body
	req.toolsJson = "{broken";
	JsonPtr k(parse(buildRequestBody(c, req)));
	REQUIRE(k.j);
	CHECK(!json_object_get(k.j, "tools"));
	CHECK(!json_object_get(k.j, "tool_choice"));
}

TEST(body_assistant_tool_calls_and_null_content) {
	Config c;
	ChatRequest req = simpleRequest();
	ChatMessage a;
	a.role = ChatMessage::ASSISTANT;
	a.contentNull = true;
	ToolCall tc;
	tc.id = "call_abc";
	tc.name = "get_patch";
	tc.arguments = "{\"x\":1}";
	a.toolCalls.push_back(tc);
	req.messages.push_back(a);
	ChatMessage t;
	t.role = ChatMessage::TOOL;
	t.toolCallId = "call_abc";
	t.content = "{\"ok\":true}";
	req.messages.push_back(t);

	JsonPtr j(parse(buildRequestBody(c, req)));
	REQUIRE(j.j);
	json_t* messages = json_object_get(j.j, "messages");
	REQUIRE((int) json_array_size(messages) == 4);
	json_t* am = json_array_get(messages, 2);
	CHECK_EQ(str(am, "role"), "assistant");
	CHECK(json_is_null(json_object_get(am, "content")));
	json_t* calls = json_object_get(am, "tool_calls");
	REQUIRE(json_is_array(calls));
	json_t* call = json_array_get(calls, 0);
	CHECK_EQ(str(call, "id"), "call_abc");
	CHECK_EQ(str(call, "type"), "function");
	json_t* fn = json_object_get(call, "function");
	CHECK_EQ(str(fn, "name"), "get_patch");
	CHECK_EQ(str(fn, "arguments"), "{\"x\":1}");
	json_t* tm = json_array_get(messages, 3);
	CHECK_EQ(str(tm, "role"), "tool");
	CHECK_EQ(str(tm, "tool_call_id"), "call_abc");
	CHECK_EQ(str(tm, "content"), "{\"ok\":true}");
}

TEST(message_null_content_only_with_tool_calls) {
	// A null content without tool calls would be rejected by providers: sent as "" instead.
	ChatMessage a;
	a.role = ChatMessage::ASSISTANT;
	a.contentNull = true;
	JsonPtr j(messageToJson(a, true));
	json_t* content = json_object_get(j.j, "content");
	REQUIRE(json_is_string(content));
	CHECK_EQ(std::string(json_string_value(content)), "");
	CHECK(!json_object_get(j.j, "tool_calls"));
}

TEST(body_reasoning_details_echo) {
	ChatRequest req = simpleRequest();
	ChatMessage a;
	a.role = ChatMessage::ASSISTANT;
	a.content = "ok";
	a.reasoning = "display only";
	a.reasoningDetailsJson = "[{\"type\":\"reasoning.encrypted\",\"data\":\"abc\",\"index\":0}]";
	req.messages.push_back(a);

	Config openrouter;
	JsonPtr j(parse(buildRequestBody(openrouter, req)));
	json_t* am = json_array_get(json_object_get(j.j, "messages"), 2);
	json_t* details = json_object_get(am, "reasoning_details");
	REQUIRE(json_is_array(details));
	CHECK_EQ(str(json_array_get(details, 0), "data"), "abc");
	// display-only reasoning is never sent
	CHECK(!json_object_get(am, "reasoning"));
	CHECK(!json_object_get(am, "reasoning_content"));

	Config openai;
	openai.reasoningParamStyle = "openai";
	JsonPtr k(parse(buildRequestBody(openai, req)));
	json_t* am2 = json_array_get(json_object_get(k.j, "messages"), 2);
	CHECK(!json_object_get(am2, "reasoning_details"));
}

TEST(body_invalid_utf8_is_sanitized) {
	Config c;
	ChatRequest req = simpleRequest();
	req.messages[1].content = std::string("bad \xFF\xFE bytes \xC3\xA4 ok");
	std::string body = buildRequestBody(c, req);
	JsonPtr j(parse(body));
	REQUIRE(j.j);
	std::string content = str(json_array_get(json_object_get(j.j, "messages"), 1), "content");
	CHECK_EQ(content, std::string("bad \xEF\xBF\xBD\xEF\xBF\xBD bytes \xC3\xA4 ok"));
}


// messageFromJson

TEST(message_roundtrip) {
	ChatMessage a;
	a.role = ChatMessage::ASSISTANT;
	a.contentNull = true;
	ToolCall tc;
	tc.id = "id1";
	tc.name = "add_module";
	tc.arguments = "{\"plugin\":\"Fundamental\"}";
	a.toolCalls.push_back(tc);
	JsonPtr j(messageToJson(a, false));
	ChatMessage b;
	REQUIRE(messageFromJson(j.j, &b));
	CHECK_EQ((int) b.role, (int) ChatMessage::ASSISTANT);
	CHECK(b.contentNull);
	REQUIRE(b.toolCalls.size() == 1);
	CHECK_EQ(b.toolCalls[0].id, "id1");
	CHECK_EQ(b.toolCalls[0].name, "add_module");
	CHECK_EQ(b.toolCalls[0].arguments, "{\"plugin\":\"Fundamental\"}");

	ChatMessage t;
	t.role = ChatMessage::TOOL;
	t.toolCallId = "id1";
	t.content = "result";
	JsonPtr tj(messageToJson(t, false));
	ChatMessage t2;
	REQUIRE(messageFromJson(tj.j, &t2));
	CHECK_EQ((int) t2.role, (int) ChatMessage::TOOL);
	CHECK_EQ(t2.toolCallId, "id1");
	CHECK_EQ(t2.content, "result");

	ChatMessage u;
	u.role = ChatMessage::USER;
	u.content = "hi";
	JsonPtr uj(messageToJson(u, false));
	ChatMessage u2;
	REQUIRE(messageFromJson(uj.j, &u2));
	CHECK_EQ((int) u2.role, (int) ChatMessage::USER);
	CHECK_EQ(u2.content, "hi");
}

TEST(message_from_json_invalid) {
	ChatMessage m;
	JsonPtr noRole(parse("{\"content\":\"x\"}"));
	CHECK(!messageFromJson(noRole.j, &m));
	JsonPtr badRole(parse("{\"role\":\"robot\",\"content\":\"x\"}"));
	CHECK(!messageFromJson(badRole.j, &m));
	JsonPtr arr(parse("[1,2]"));
	CHECK(!messageFromJson(arr.j, &m));
	CHECK(!messageFromJson(NULL, &m));
}


// Response parsing

TEST(response_text_only) {
	const char* body = "{\"id\":\"x\",\"model\":\"openai/gpt-5.6-terra\",\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
		"\"message\":{\"role\":\"assistant\",\"content\":\"Hello there\"}}],"
		"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15,"
		"\"completion_tokens_details\":{\"reasoning_tokens\":3},\"cost\":0.00123}}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::NONE);
	CHECK_EQ(r.message.content, "Hello there");
	CHECK(!r.message.contentNull);
	CHECK_EQ((int) r.message.role, (int) ChatMessage::ASSISTANT);
	CHECK(r.message.toolCalls.empty());
	CHECK_EQ(r.finishReason, "stop");
	CHECK_EQ(r.model, "openai/gpt-5.6-terra");
	CHECK_EQ(r.usage.promptTokens, 10);
	CHECK_EQ(r.usage.completionTokens, 5);
	CHECK_EQ(r.usage.totalTokens, 15);
	CHECK_EQ(r.usage.reasoningTokens, 3);
	CHECK(r.usage.cost > 0.00122 && r.usage.cost < 0.00124);
}

TEST(response_usage_missing) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\"}}]}");
	REQUIRE(r.ok);
	CHECK_EQ(r.usage.promptTokens, -1);
	CHECK_EQ(r.usage.reasoningTokens, -1);
	CHECK(r.usage.cost < 0);
	CHECK_EQ(r.finishReason, "");
}

TEST(response_null_content_and_tool_calls) {
	const char* body = "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,"
		"\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"{}\"}}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	CHECK(r.message.contentNull);
	CHECK_EQ(r.message.content, "");
	REQUIRE(r.message.toolCalls.size() == 1);
	CHECK_EQ(r.message.toolCalls[0].id, "call_1");
	CHECK_EQ(r.message.toolCalls[0].name, "get_patch");
	CHECK_EQ(r.message.toolCalls[0].arguments, "{}");
	CHECK_EQ(r.finishReason, "tool_calls");
}

TEST(response_missing_content_key) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"a\",\"function\":{\"name\":\"n\",\"arguments\":\"{}\"}}]}}]}");
	REQUIRE(r.ok);
	CHECK(r.message.contentNull);
	CHECK_EQ((int) r.message.toolCalls.size(), 1);
}

TEST(response_multiple_tool_calls) {
	const char* body = "{\"choices\":[{\"message\":{\"content\":\"Working\",\"tool_calls\":["
		"{\"id\":\"c1\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"{}\"}},"
		"{\"id\":\"c2\",\"function\":{\"name\":\"search_modules\",\"arguments\":\"{\\\"query\\\":\\\"VCO\\\"}\"}}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	CHECK_EQ(r.message.content, "Working");
	REQUIRE(r.message.toolCalls.size() == 2);
	CHECK_EQ(r.message.toolCalls[1].id, "c2");
	CHECK_EQ(r.message.toolCalls[1].name, "search_modules");
	CHECK_EQ(r.message.toolCalls[1].arguments, "{\"query\":\"VCO\"}");
}

TEST(response_content_array) {
	const char* body = "{\"choices\":[{\"message\":{\"content\":["
		"{\"type\":\"text\",\"text\":\"Hello \"},{\"type\":\"image\",\"text\":\"ignored\"},"
		"{\"type\":\"output_text\",\"text\":\"world\"}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	CHECK_EQ(r.message.content, "Hello world");
	CHECK(!r.message.contentNull);
}

TEST(response_object_arguments) {
	const char* body = "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":["
		"{\"id\":\"c1\",\"function\":{\"name\":\"add_module\",\"arguments\":{\"plugin\":\"Fundamental\",\"model\":\"VCO\"}}},"
		"{\"id\":\"c2\",\"function\":{\"name\":\"x\",\"arguments\":[1,2]}},"
		"{\"id\":\"c3\",\"function\":{\"name\":\"y\"}},"
		"{\"id\":\"c4\",\"function\":{\"name\":\"z\",\"arguments\":null}}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 4);
	CHECK_EQ(r.message.toolCalls[0].arguments, "{\"plugin\":\"Fundamental\",\"model\":\"VCO\"}");
	CHECK_EQ(r.message.toolCalls[1].arguments, "[1,2]");
	CHECK_EQ(r.message.toolCalls[2].arguments, "{}");
	CHECK_EQ(r.message.toolCalls[3].arguments, "{}");
}

TEST(response_invalid_arguments_kept_verbatim) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"c\",\"function\":{\"name\":\"add_module\",\"arguments\":\"{not json\"}}]}}]}");
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 1);
	CHECK_EQ(r.message.toolCalls[0].arguments, "{not json");
}

TEST(response_empty_arguments_become_empty_object) {
	const char* body = "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":["
		"{\"id\":\"a\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"\"}},"
		"{\"id\":\"b\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"  \\n\"}},"
		"{\"id\":\"c\",\"function\":{\"name\":\"get_patch\",\"arguments\":\" {\\\"x\\\":1} \"}}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 3);
	CHECK_EQ(r.message.toolCalls[0].arguments, "{}");
	CHECK_EQ(r.message.toolCalls[1].arguments, "{}");
	// Non-empty strings stay verbatim (including surrounding whitespace)
	CHECK_EQ(r.message.toolCalls[2].arguments, " {\"x\":1} ");
	// Legacy function_call
	ChatResponse l = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"function_call\":{\"name\":\"get_patch\",\"arguments\":\"\"}}}]}");
	REQUIRE(l.ok);
	REQUIRE(l.message.toolCalls.size() == 1);
	CHECK_EQ(l.message.toolCalls[0].arguments, "{}");
	// The echoed request carries valid JSON arguments
	JsonPtr j(messageToJson(r.message, false));
	CHECK(json_string_value(json_object_get(json_object_get(json_array_get(json_object_get(j.j, "tool_calls"), 0), "function"), "arguments")) == std::string("{}"));
}

TEST(response_refusal_becomes_content) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"refusal\":\"I can't help with that.\"},\"finish_reason\":\"stop\"}]}");
	REQUIRE(r.ok);
	CHECK_EQ(r.message.content, "I can't help with that.");
	CHECK(!r.message.contentNull);
	// Normal content wins over a refusal field
	ChatResponse a = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"hi\",\"refusal\":\"no\"}}]}");
	CHECK_EQ(a.message.content, "hi");
	// Tool calls win too: content stays null
	ChatResponse b = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"refusal\":\"no\",\"tool_calls\":[{\"id\":\"c\",\"function\":{\"name\":\"get_patch\",\"arguments\":\"{}\"}}]}}]}");
	CHECK_EQ(b.message.content, "");
	CHECK(b.message.contentNull);
	// A null refusal changes nothing
	ChatResponse c = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"refusal\":null}}]}");
	CHECK_EQ(c.message.content, "");
	CHECK(c.message.contentNull);
}

TEST(response_usage_out_of_range_numbers) {
	// Huge reals must not hit undefined behavior in the double -> int conversion
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\"}}],\"usage\":{\"prompt_tokens\":1e30,\"completion_tokens\":-1e30,\"total_tokens\":12.9}}");
	REQUIRE(r.ok);
	CHECK_EQ(r.usage.promptTokens, INT_MAX);
	CHECK_EQ(r.usage.completionTokens, INT_MIN);
	CHECK_EQ(r.usage.totalTokens, 12);
	ChatResponse i = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\"}}],\"usage\":{\"prompt_tokens\":99999999999999}}");
	CHECK_EQ(i.usage.promptTokens, INT_MAX);
}

TEST(response_missing_tool_call_ids) {
	const char* body = "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":["
		"{\"function\":{\"name\":\"a\",\"arguments\":\"{}\"}},"
		"{\"id\":\"\",\"function\":{\"name\":\"b\",\"arguments\":\"{}\"}},"
		"{\"id\":\"keep\",\"function\":{\"name\":\"c\",\"arguments\":\"{}\"}}]}}]}";
	ChatResponse r = parseResponse(200, body);
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 3);
	CHECK_EQ(r.message.toolCalls[0].id, "call_0");
	CHECK_EQ(r.message.toolCalls[1].id, "call_1");
	CHECK_EQ(r.message.toolCalls[2].id, "keep");
}

TEST(response_tool_call_without_name_is_kept) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"c\",\"function\":{\"arguments\":\"{}\"}},{\"id\":\"d\"}]}}]}");
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 2);
	CHECK_EQ(r.message.toolCalls[0].name, "");
	CHECK_EQ(r.message.toolCalls[1].name, "");
	CHECK_EQ(r.message.toolCalls[1].arguments, "{}");
}

TEST(response_legacy_function_call) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"finish_reason\":\"function_call\",\"message\":{\"content\":null,\"function_call\":{\"name\":\"get_patch\",\"arguments\":\"{\\\"a\\\":1}\"}}}]}");
	REQUIRE(r.ok);
	REQUIRE(r.message.toolCalls.size() == 1);
	CHECK_EQ(r.message.toolCalls[0].id, "call_0");
	CHECK_EQ(r.message.toolCalls[0].name, "get_patch");
	CHECK_EQ(r.message.toolCalls[0].arguments, "{\"a\":1}");
}

TEST(response_reasoning_fields) {
	ChatResponse a = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\",\"reasoning\":\"thinking...\"}}]}");
	REQUIRE(a.ok);
	CHECK_EQ(a.message.reasoning, "thinking...");
	CHECK_EQ(a.message.reasoningDetailsJson, "");

	ChatResponse b = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\",\"reasoning_content\":\"deepseek style\"}}]}");
	REQUIRE(b.ok);
	CHECK_EQ(b.message.reasoning, "deepseek style");

	// reasoning_details: dumped verbatim (compact) and used to derive the display text
	const char* body = "{\"choices\":[{\"message\":{\"content\":\"x\",\"reasoning_details\":["
		"{\"type\":\"reasoning.summary\",\"summary\":\"Summary text\",\"index\":0},"
		"{\"type\":\"reasoning.text\",\"text\":\"Some thoughts\",\"index\":1},"
		"{\"type\":\"reasoning.encrypted\",\"data\":\"zzz\",\"index\":2}]}}]}";
	ChatResponse c = parseResponse(200, body);
	REQUIRE(c.ok);
	CHECK_EQ(c.message.reasoning, "Summary text\nSome thoughts");
	JsonPtr details(parse(c.message.reasoningDetailsJson));
	REQUIRE(json_is_array(details.j));
	CHECK_EQ((int) json_array_size(details.j), 3);
	CHECK_EQ(str(json_array_get(details.j, 2), "data"), "zzz");

	// An explicit reasoning string wins over the derived text
	ChatResponse d = parseResponse(200, "{\"choices\":[{\"message\":{\"content\":\"x\",\"reasoning\":\"explicit\",\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"derived\"}]}}]}");
	REQUIRE(d.ok);
	CHECK_EQ(d.message.reasoning, "explicit");
	CHECK(!d.message.reasoningDetailsJson.empty());

	// Echo: a parsed message goes back verbatim as reasoning_details
	ChatRequest req = simpleRequest();
	req.messages.push_back(c.message);
	Config cfg;
	JsonPtr j(parse(buildRequestBody(cfg, req)));
	json_t* am = json_array_get(json_object_get(j.j, "messages"), 2);
	json_t* echoed = json_object_get(am, "reasoning_details");
	REQUIRE(json_is_array(echoed));
	CHECK(json_equal(echoed, details.j));
}

TEST(response_200_with_error) {
	ChatResponse r = parseResponse(200, "{\"error\":{\"code\":402,\"message\":\"Insufficient credits\"}}");
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::PAYMENT);
	CHECK_EQ(r.error.httpStatus, 402);

	ChatResponse s = parseResponse(200, "{\"error\":{\"code\":\"weird\",\"message\":\"Something odd\"}}");
	CHECK(!s.ok);
	CHECK_EQ((int) s.error.kind, (int) LlmError::OTHER);
	CHECK_EQ(s.error.detail, "Something odd");
	CHECK(s.error.message.find("Something odd") != std::string::npos);

	ChatResponse t = parseResponse(200, "{\"error\":{\"message\":\"no code\"}}");
	CHECK(!t.ok);
	CHECK_EQ((int) t.error.kind, (int) LlmError::OTHER);

	ChatResponse u = parseResponse(200, "{\"error\":\"plain string error\"}");
	CHECK(!u.ok);
	CHECK_EQ(u.error.detail, "plain string error");

	// A non-HTTP-like code (e.g. 1) maps to OTHER
	ChatResponse v = parseResponse(200, "{\"error\":{\"code\":1,\"message\":\"m\"}}");
	CHECK_EQ((int) v.error.kind, (int) LlmError::OTHER);
}

TEST(response_choice_error) {
	ChatResponse r = parseResponse(200, "{\"choices\":[{\"error\":{\"code\":429,\"message\":\"upstream limited\"},\"message\":{\"content\":\"\"}}]}");
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::RATE_LIMIT);
	CHECK_EQ(r.error.detail, "upstream limited");
}

TEST(response_missing_choices) {
	ChatResponse a = parseResponse(200, "{\"id\":\"x\"}");
	CHECK(!a.ok);
	CHECK_EQ((int) a.error.kind, (int) LlmError::BAD_RESPONSE);
	ChatResponse b = parseResponse(200, "{\"choices\":[]}");
	CHECK(!b.ok);
	CHECK_EQ((int) b.error.kind, (int) LlmError::BAD_RESPONSE);
	ChatResponse c = parseResponse(200, "{\"choices\":\"nope\"}");
	CHECK(!c.ok);
	CHECK_EQ((int) c.error.kind, (int) LlmError::BAD_RESPONSE);
	ChatResponse d = parseResponse(200, "{\"choices\":[{\"finish_reason\":\"stop\"}]}");
	CHECK(!d.ok);
	CHECK_EQ((int) d.error.kind, (int) LlmError::BAD_RESPONSE);
}

TEST(response_invalid_json) {
	ChatResponse a = parseResponse(200, "<html>proxy error</html>");
	CHECK(!a.ok);
	CHECK_EQ((int) a.error.kind, (int) LlmError::BAD_RESPONSE);
	CHECK_EQ(a.error.message, "The server returned invalid JSON");
	ChatResponse b = parseResponse(200, "");
	CHECK(!b.ok);
	CHECK_EQ((int) b.error.kind, (int) LlmError::BAD_RESPONSE);
	ChatResponse c = parseResponse(200, "[1,2,3]");
	CHECK(!c.ok);
	CHECK_EQ((int) c.error.kind, (int) LlmError::BAD_RESPONSE);
}


// HTTP errors

TEST(http_error_table) {
	const char* body = "{\"error\":{\"message\":\"The detail\"}}";
	struct Row {
		long status;
		LlmError::Kind kind;
		const char* message;
	};
	const Row rows[] = {
		{400, LlmError::BAD_REQUEST, "The provider rejected the request (400): The detail. Check model parameters (temperature, max_tokens, reasoning) in the assistant settings."},
		{401, LlmError::AUTH, "Authentication failed (401). Check your API key (RACK_ASSISTANT_API_KEY / OPENROUTER_API_KEY or the assistant settings)."},
		{402, LlmError::PAYMENT, "Insufficient credits (402). Top up your account or pick a cheaper model."},
		{403, LlmError::FORBIDDEN, "Access denied (403): The detail"},
		{404, LlmError::NOT_FOUND, "Model or endpoint not found (404). Check 'model' and 'base_url' in the assistant settings."},
		{408, LlmError::TIMEOUT, "The provider timed out (408). Try again."},
		{413, LlmError::BAD_REQUEST, "Request too large (413). Start a new chat to shorten the conversation."},
		{429, LlmError::RATE_LIMIT, "Rate limited (429). Wait a moment and try again."},
		{500, LlmError::SERVER, "The provider had a server error (500). Try again later."},
		{502, LlmError::SERVER, "The provider had a server error (502). Try again later."},
		{503, LlmError::SERVER, "The provider had a server error (503). Try again later."},
		{599, LlmError::SERVER, "The provider had a server error (599). Try again later."},
		{418, LlmError::OTHER, "Unexpected HTTP status 418: The detail"},
		{302, LlmError::OTHER, "Unexpected HTTP status 302: The detail. The server redirected the request; check that 'base_url' is correct (for hosted providers use https://)."},
	};
	for (const Row& row : rows) {
		LlmError e = httpError(row.status, body);
		CHECK_EQ((int) e.kind, (int) row.kind);
		CHECK_EQ(e.message, row.message);
		CHECK_EQ(e.httpStatus, (int) row.status);
		CHECK_EQ(e.detail, "The detail");
		// Also reachable through parseResponse for non-2xx statuses
		ChatResponse r = parseResponse(row.status, body);
		CHECK(!r.ok);
		CHECK_EQ((int) r.error.kind, (int) row.kind);
	}
}

TEST(http_error_detail_sources) {
	// error as string
	CHECK_EQ(httpError(400, "{\"error\":\"string error\"}").detail, "string error");
	// raw body
	CHECK_EQ(httpError(500, "  Bad gateway  ").detail, "Bad gateway");
	// empty body
	LlmError e = httpError(400, "");
	CHECK_EQ(e.detail, "");
	CHECK_EQ(e.message, "The provider rejected the request (400). Check model parameters (temperature, max_tokens, reasoning) in the assistant settings.");
	// no double period
	LlmError p = httpError(400, "{\"error\":{\"message\":\"Unsupported parameter.\"}}");
	CHECK_EQ(p.detail, "Unsupported parameter.");
	CHECK(p.message.find("..") == std::string::npos);
	// OpenRouter metadata.raw is appended
	LlmError m = httpError(400, "{\"error\":{\"message\":\"Provider returned error\",\"code\":400,\"metadata\":{\"raw\":\"upstream said no\"}}}");
	CHECK_EQ(m.detail, "Provider returned error | upstream said no");
}

TEST(http_error_json_without_error_field_uses_raw_body) {
	// FastAPI / vLLM / LM Studio style bodies carry neither "error" nor "message"
	const char* body = "{\"detail\":[{\"msg\":\"bad\"}]}";
	LlmError e = httpError(422, body);
	CHECK_EQ(e.detail, body);
	CHECK_EQ(e.message, std::string("Unexpected HTTP status 422: ") + body);
	LlmError f = httpError(403, "{\"detail\":\"Not allowed\"}");
	CHECK_EQ(f.message, "Access denied (403): {\"detail\":\"Not allowed\"}");
	// An empty JSON object has nothing to show
	CHECK_EQ(httpError(400, "{}").detail, "{}");
	// "message" still wins when present
	CHECK_EQ(httpError(400, "{\"message\":\"top-level message\"}").detail, "top-level message");
	// A 200 response with an empty error string does not dump the whole body
	ChatResponse r = parseResponse(200, "{\"error\":\"\",\"choices\":[]}");
	CHECK(!r.ok);
	CHECK_EQ(r.error.detail, "");
}

TEST(http_error_trailing_periods_stripped_in_all_messages) {
	LlmError a = httpError(403, "{\"error\":{\"message\":\"Access denied.\"}}");
	CHECK_EQ(a.message, "Access denied (403): Access denied");
	CHECK_EQ(a.detail, "Access denied.");
	LlmError b = httpError(418, "{\"error\":{\"message\":\"I'm a teapot.\"}}");
	CHECK_EQ(b.message, "Unexpected HTTP status 418: I'm a teapot");
	// Only periods/spaces: nothing is appended
	LlmError c = httpError(418, "{\"error\":{\"message\":\"  . . \"}}");
	CHECK_EQ(c.message, "Unexpected HTTP status 418");
	LlmError d = httpError(403, "{\"error\":{\"message\":\" ... \"}}");
	CHECK_EQ(d.message, "Access denied (403).");
}

TEST(http_error_detail_collapses_whitespace) {
	LlmError e = httpError(502, "<html>\n\t<head>\r\n  <title>Bad   Gateway</title>\n</head>\n</html>\n");
	CHECK_EQ(e.detail, "<html> <head> <title>Bad Gateway</title> </head> </html>");
	CHECK(e.message.find('\n') == std::string::npos);
	LlmError f = httpError(418, "line one\n\nline two");
	CHECK_EQ(f.message, "Unexpected HTTP status 418: line one line two");
}

TEST(http_error_redirect_hint) {
	LlmError e = httpError(301, "");
	CHECK_EQ((int) e.kind, (int) LlmError::OTHER);
	CHECK(e.message.find("Unexpected HTTP status 301") == 0);
	CHECK(e.message.find("base_url") != std::string::npos);
	LlmError f = httpError(418, "");
	CHECK_EQ(f.message, "Unexpected HTTP status 418");
}

TEST(http_error_detail_truncated) {
	std::string longMsg(1000, 'a');
	LlmError e = httpError(403, "{\"error\":{\"message\":\"" + longMsg + "\"}}");
	CHECK_EQ((int) e.detail.size(), 300 + 3); // 300 bytes + "…"
	CHECK(e.detail.compare(300, 3, "\xE2\x80\xA6") == 0);
	LlmError r = httpError(500, std::string(5000, 'x'));
	CHECK((int) r.detail.size() <= 303);
}

TEST(http_error_401_message_has_no_detail) {
	// Messages never echo the request; only the provider detail is included.
	LlmError e = httpError(401, "{\"error\":{\"message\":\"Invalid key sk-or-v1-xxxx\"}}");
	CHECK(e.message.find("sk-or") == std::string::npos);
}


// truncateUtf8

TEST(truncate_utf8) {
	CHECK_EQ(truncateUtf8("hello", 10), "hello");
	CHECK_EQ(truncateUtf8("hello", 5), "hello");
	CHECK_EQ(truncateUtf8("hello world", 5), "hello\xE2\x80\xA6");
	CHECK_EQ(truncateUtf8("hello", 0), "\xE2\x80\xA6");
	// "ä" is 2 bytes: cutting in the middle backs up to the boundary
	std::string s = "a\xC3\xA4" "b";
	CHECK_EQ(truncateUtf8(s, 2), "a\xE2\x80\xA6");
	CHECK_EQ(truncateUtf8(s, 3), "a\xC3\xA4\xE2\x80\xA6");
	// 4 byte emoji
	std::string e = "x\xF0\x9F\x98\x80y";
	CHECK_EQ(truncateUtf8(e, 2), "x\xE2\x80\xA6");
	CHECK_EQ(truncateUtf8(e, 3), "x\xE2\x80\xA6");
	CHECK_EQ(truncateUtf8(e, 4), "x\xE2\x80\xA6");
	CHECK_EQ(truncateUtf8(e, 5), "x\xF0\x9F\x98\x80\xE2\x80\xA6");
}

TEST(sanitize_utf8) {
	CHECK_EQ(sanitizeUtf8("plain"), "plain");
	CHECK_EQ(sanitizeUtf8("\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80"), "\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80");
	CHECK_EQ(sanitizeUtf8("a\xFF" "b"), "a\xEF\xBF\xBD" "b");
	// truncated sequence at the end
	CHECK_EQ(sanitizeUtf8("a\xE2\x82"), "a\xEF\xBF\xBD\xEF\xBF\xBD");
	// overlong and surrogate encodings
	CHECK_EQ(sanitizeUtf8("\xC0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD");
	CHECK_EQ(sanitizeUtf8("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

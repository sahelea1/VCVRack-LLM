#include <assistant/Protocol.hpp>
#include <string.hpp>

#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdlib>


namespace rack {
namespace assistant {


static const size_t DETAIL_MAX_BYTES = 300;


// UTF-8 helpers

/** Returns the length of the valid UTF-8 sequence starting at s[i], or 0 if invalid. */
static size_t utf8SequenceLength(const std::string& s, size_t i) {
	unsigned char c = (unsigned char) s[i];
	size_t n = s.size();
	if (c < 0x80)
		return 1;
	if (c >= 0xC2 && c <= 0xDF) {
		if (i + 1 < n && ((unsigned char) s[i + 1] & 0xC0) == 0x80)
			return 2;
		return 0;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		if (i + 2 >= n)
			return 0;
		unsigned char c1 = (unsigned char) s[i + 1];
		unsigned char c2 = (unsigned char) s[i + 2];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
			return 0;
		if (c == 0xE0 && c1 < 0xA0)
			return 0; // overlong
		if (c == 0xED && c1 >= 0xA0)
			return 0; // surrogate
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		if (i + 3 >= n)
			return 0;
		unsigned char c1 = (unsigned char) s[i + 1];
		unsigned char c2 = (unsigned char) s[i + 2];
		unsigned char c3 = (unsigned char) s[i + 3];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
			return 0;
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
		size_t len = utf8SequenceLength(s, i);
		if (len == 0) {
			out += "\xEF\xBF\xBD";
			i++;
		}
		else {
			out.append(s, i, len);
			i += len;
		}
	}
	return out;
}


json_t* jsonString(const std::string& s) {
	std::string clean = sanitizeUtf8(s);
	json_t* j = json_stringn(clean.data(), clean.size());
	if (!j)
		j = json_string("");
	return j;
}


std::string truncateUtf8(const std::string& s, size_t maxBytes) {
	if (s.size() <= maxBytes)
		return s;
	size_t cut = maxBytes;
	// Back up to a code point boundary (s[cut] must not be a continuation byte)
	while (cut > 0 && ((unsigned char) s[cut] & 0xC0) == 0x80)
		cut--;
	return s.substr(0, cut) + "\xE2\x80\xA6";
}


static std::string dumpCompact(json_t* j, size_t flags = 0) {
	char* dump = json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY | flags);
	if (!dump)
		return "";
	std::string s = dump;
	std::free(dump);
	return s;
}


static std::string getString(json_t* obj, const char* key) {
	json_t* j = json_object_get(obj, key);
	if (j && json_is_string(j))
		return std::string(json_string_value(j), json_string_length(j));
	return "";
}


/** Reads an int from an integer or real JSON value. */
static bool getInt(json_t* obj, const char* key, int* out) {
	json_t* j = json_object_get(obj, key);
	if (!j)
		return false;
	if (json_is_integer(j)) {
		json_int_t v = json_integer_value(j);
		*out = (int) std::max<json_int_t>(std::min<json_int_t>(v, INT_MAX), INT_MIN);
		return true;
	}
	if (json_is_real(j) && std::isfinite(json_real_value(j))) {
		// Clamp before casting, an out-of-range double to int conversion is undefined behavior
		double v = json_real_value(j);
		if (v >= (double) INT_MAX)
			*out = INT_MAX;
		else if (v <= (double) INT_MIN)
			*out = INT_MIN;
		else
			*out = (int) v;
		return true;
	}
	return false;
}


// URL and request body

std::string chatCompletionsUrl(const std::string& baseUrl) {
	std::string base = string::trim(baseUrl);
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	const std::string suffix = "/chat/completions";
	if (string::endsWith(base, suffix))
		return base;
	return base + suffix;
}


json_t* messageToJson(const ChatMessage& m, bool includeReasoningDetails) {
	json_t* j = json_object();
	switch (m.role) {
		case ChatMessage::SYSTEM:
			json_object_set_new(j, "role", json_string("system"));
			json_object_set_new(j, "content", jsonString(m.content));
		break;
		case ChatMessage::USER:
			json_object_set_new(j, "role", json_string("user"));
			json_object_set_new(j, "content", jsonString(m.content));
		break;
		case ChatMessage::ASSISTANT: {
			json_object_set_new(j, "role", json_string("assistant"));
			// A null content is only valid together with tool calls (providers reject it otherwise).
			if (m.contentNull && m.content.empty() && !m.toolCalls.empty())
				json_object_set_new(j, "content", json_null());
			else
				json_object_set_new(j, "content", jsonString(m.content));
			if (!m.toolCalls.empty()) {
				json_t* callsJ = json_array();
				for (const ToolCall& tc : m.toolCalls) {
					json_t* fnJ = json_object();
					json_object_set_new(fnJ, "name", jsonString(tc.name));
					json_object_set_new(fnJ, "arguments", jsonString(tc.arguments));
					json_t* callJ = json_object();
					json_object_set_new(callJ, "id", jsonString(tc.id));
					json_object_set_new(callJ, "type", json_string("function"));
					json_object_set_new(callJ, "function", fnJ);
					json_array_append_new(callsJ, callJ);
				}
				json_object_set_new(j, "tool_calls", callsJ);
			}
			if (includeReasoningDetails && !m.reasoningDetailsJson.empty()) {
				json_error_t error;
				json_t* detailsJ = json_loadb(m.reasoningDetailsJson.data(), m.reasoningDetailsJson.size(), 0, &error);
				if (detailsJ)
					json_object_set_new(j, "reasoning_details", detailsJ);
			}
		} break;
		case ChatMessage::TOOL:
			json_object_set_new(j, "role", json_string("tool"));
			json_object_set_new(j, "tool_call_id", jsonString(m.toolCallId));
			json_object_set_new(j, "content", jsonString(m.content));
		break;
	}
	return j;
}


std::string buildRequestBody(const Config& c, const ChatRequest& req) {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "model", jsonString(c.model));

	bool openrouterStyle = (c.reasoningParamStyle != "openai");
	json_t* messagesJ = json_array();
	for (const ChatMessage& m : req.messages)
		json_array_append_new(messagesJ, messageToJson(m, openrouterStyle));
	json_object_set_new(rootJ, "messages", messagesJ);

	if (!req.toolsJson.empty()) {
		json_error_t error;
		json_t* toolsJ = json_loadb(req.toolsJson.data(), req.toolsJson.size(), 0, &error);
		if (toolsJ && json_is_array(toolsJ)) {
			json_object_set_new(rootJ, "tools", toolsJ);
			json_object_set_new(rootJ, "tool_choice", json_string("auto"));
		}
		else if (toolsJ) {
			json_decref(toolsJ);
		}
	}

	// Reasoning: "off" sends no reasoning parameter at all (non-reasoning models)
	if (c.reasoningEffort != "off" && !c.reasoningEffort.empty()) {
		if (openrouterStyle) {
			json_t* reasoningJ = json_object();
			json_object_set_new(reasoningJ, "effort", jsonString(c.reasoningEffort));
			json_object_set_new(rootJ, "reasoning", reasoningJ);
		}
		else {
			json_object_set_new(rootJ, "reasoning_effort", jsonString(c.reasoningEffort));
		}
	}

	if (c.maxTokens > 0)
		json_object_set_new(rootJ, openrouterStyle ? "max_tokens" : "max_completion_tokens", json_integer(c.maxTokens));

	if (c.hasTemperature)
		json_object_set_new(rootJ, "temperature", json_real(temperatureToDouble(c.temperature)));

	// 15 significant digits keep decimals like 0.7 as typed (the default of 17 prints 0.69999999999999996)
	std::string body = dumpCompact(rootJ, JSON_REAL_PRECISION(15));
	json_decref(rootJ);
	return body;
}


// Response parsing

/** Concatenates the text of a content parts array. */
static std::string contentPartsToString(json_t* partsJ) {
	std::string text;
	size_t i;
	json_t* partJ;
	json_array_foreach(partsJ, i, partJ) {
		if (json_is_string(partJ)) {
			text += std::string(json_string_value(partJ), json_string_length(partJ));
		}
		else if (json_is_object(partJ)) {
			std::string type = getString(partJ, "type");
			if (type == "text" || type == "output_text")
				text += getString(partJ, "text");
		}
	}
	return text;
}


/** Tool call arguments as sent back to the provider: strings are kept verbatim, except that
empty/whitespace-only strings (some providers emit them for no-argument tools) become "{}". */
static std::string argumentsToString(json_t* argsJ) {
	if (argsJ && json_is_string(argsJ)) {
		std::string args(json_string_value(argsJ), json_string_length(argsJ));
		if (string::trim(args).empty())
			return "{}";
		return args;
	}
	if (argsJ && !json_is_null(argsJ))
		return dumpCompact(argsJ);
	return "{}";
}


/** Parses an OpenAI-style message object (assistant response or persisted message). */
static bool parseMessageObject(json_t* msgJ, ChatMessage* out) {
	if (!msgJ || !json_is_object(msgJ))
		return false;
	ChatMessage m;

	std::string role = getString(msgJ, "role");
	if (role.empty() || role == "assistant")
		m.role = ChatMessage::ASSISTANT;
	else if (role == "system")
		m.role = ChatMessage::SYSTEM;
	else if (role == "user")
		m.role = ChatMessage::USER;
	else if (role == "tool")
		m.role = ChatMessage::TOOL;
	else
		return false;

	// Content
	json_t* contentJ = json_object_get(msgJ, "content");
	if (contentJ && json_is_string(contentJ)) {
		m.content = std::string(json_string_value(contentJ), json_string_length(contentJ));
	}
	else if (contentJ && json_is_array(contentJ)) {
		m.content = contentPartsToString(contentJ);
	}
	else {
		// null or missing
		m.contentNull = true;
	}

	m.toolCallId = getString(msgJ, "tool_call_id");

	// Tool calls
	json_t* callsJ = json_object_get(msgJ, "tool_calls");
	if (callsJ && json_is_array(callsJ)) {
		size_t i;
		json_t* callJ;
		json_array_foreach(callsJ, i, callJ) {
			if (!json_is_object(callJ))
				continue;
			ToolCall tc;
			// A missing id stays empty: the controller assigns ids that are unique across rounds
			tc.id = getString(callJ, "id");
			json_t* fnJ = json_object_get(callJ, "function");
			tc.arguments = "{}";
			if (fnJ && json_is_object(fnJ)) {
				tc.name = getString(fnJ, "name");
				tc.arguments = argumentsToString(json_object_get(fnJ, "arguments"));
			}
			m.toolCalls.push_back(tc);
		}
	}
	// Legacy function_call
	if (m.toolCalls.empty()) {
		json_t* fcJ = json_object_get(msgJ, "function_call");
		if (fcJ && json_is_object(fcJ)) {
			ToolCall tc;
			tc.name = getString(fcJ, "name");
			tc.arguments = argumentsToString(json_object_get(fcJ, "arguments"));
			m.toolCalls.push_back(tc);
		}
	}

	// Reasoning
	m.reasoning = getString(msgJ, "reasoning");
	if (m.reasoning.empty())
		m.reasoning = getString(msgJ, "reasoning_content");
	json_t* detailsJ = json_object_get(msgJ, "reasoning_details");
	if (detailsJ && json_is_array(detailsJ) && json_array_size(detailsJ) > 0) {
		m.reasoningDetailsJson = dumpCompact(detailsJ);
		if (m.reasoning.empty()) {
			size_t i;
			json_t* entryJ;
			json_array_foreach(detailsJ, i, entryJ) {
				if (!json_is_object(entryJ))
					continue;
				std::string t = getString(entryJ, "text");
				if (t.empty())
					t = getString(entryJ, "summary");
				if (t.empty())
					continue;
				if (!m.reasoning.empty())
					m.reasoning += "\n";
				m.reasoning += t;
			}
		}
	}

	*out = m;
	return true;
}


bool messageFromJson(json_t* j, ChatMessage* out) {
	if (!j || !json_is_object(j) || !out)
		return false;
	// Persisted messages must carry an explicit, valid role
	if (getString(j, "role").empty())
		return false;
	return parseMessageObject(j, out);
}


/** Replaces runs of whitespace and control characters by a single space and trims the result,
so multi-line bodies (e.g. HTML error pages) stay on one line in user-facing messages. */
static std::string collapseWhitespace(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	bool pendingSpace = false;
	for (char ch : s) {
		unsigned char c = (unsigned char) ch;
		if (c <= 0x20 || c == 0x7F) {
			pendingSpace = !out.empty();
		}
		else {
			if (pendingSpace)
				out += ' ';
			pendingSpace = false;
			out += ch;
		}
	}
	return out;
}


/** Extracts the provider's error text from a parsed body or error object.
If rawFallback is set and the body yields no text (not JSON, or a JSON object without
"error"/"message", e.g. {"detail": [...]}), the raw body is used. */
static std::string extractDetail(json_t* rootJ, const std::string& rawBody, bool rawFallback) {
	std::string detail;
	if (rootJ && json_is_object(rootJ)) {
		json_t* errJ = json_object_get(rootJ, "error");
		if (errJ && json_is_object(errJ)) {
			detail = getString(errJ, "message");
			// OpenRouter puts the upstream provider's text into metadata.raw
			json_t* metaJ = json_object_get(errJ, "metadata");
			if (metaJ && json_is_object(metaJ)) {
				json_t* rawJ = json_object_get(metaJ, "raw");
				std::string raw;
				if (rawJ && json_is_string(rawJ))
					raw = json_string_value(rawJ);
				else if (rawJ && !json_is_null(rawJ))
					raw = dumpCompact(rawJ);
				if (!raw.empty())
					detail += (detail.empty() ? "" : " | ") + raw;
			}
			if (detail.empty())
				detail = dumpCompact(errJ);
		}
		else if (errJ && json_is_string(errJ)) {
			detail = getString(rootJ, "error");
		}
		else {
			detail = getString(rootJ, "message");
		}
	}
	if (detail.empty() && rawFallback)
		detail = rawBody;
	return truncateUtf8(collapseWhitespace(sanitizeUtf8(detail)), DETAIL_MAX_BYTES);
}


/** Strips trailing periods so messages don't end up with ".." */
static std::string stripTrailingPeriods(std::string s) {
	while (!s.empty() && (s.back() == '.' || s.back() == ' '))
		s.pop_back();
	return s;
}


static LlmError makeHttpError(long status, const std::string& detail) {
	LlmError e;
	e.httpStatus = (int) status;
	e.detail = detail;
	std::string d = stripTrailingPeriods(detail);
	std::string st = std::to_string(status);
	if (status == 400) {
		e.kind = LlmError::BAD_REQUEST;
		// "openai/foo is not a valid model ID": point at the model name instead of the sampling parameters
		std::string dl = d;
		for (size_t i = 0; i < dl.size(); i++)
			dl[i] = (char) std::tolower((unsigned char) dl[i]);
		bool modelProblem = dl.find("model") != std::string::npos && (dl.find("not a valid") != std::string::npos || dl.find("not found") != std::string::npos || dl.find("does not exist") != std::string::npos || dl.find("invalid model") != std::string::npos);
		e.message = "The provider rejected the request (400)" + (d.empty() ? std::string() : ": " + d) + (modelProblem ? ". Check 'model' in the assistant settings." : ". Check model parameters (temperature, max_tokens, reasoning) in the assistant settings.");
	}
	else if (status == 401) {
		e.kind = LlmError::AUTH;
		e.message = "Authentication failed (401). Check your API key (RACK_ASSISTANT_API_KEY / OPENROUTER_API_KEY or the assistant settings).";
	}
	else if (status == 402) {
		e.kind = LlmError::PAYMENT;
		e.message = "Insufficient credits (402). Top up your account or pick a cheaper model.";
	}
	else if (status == 403) {
		e.kind = LlmError::FORBIDDEN;
		e.message = "Access denied (403)" + (d.empty() ? std::string(".") : ": " + d);
	}
	else if (status == 404) {
		e.kind = LlmError::NOT_FOUND;
		e.message = "Model or endpoint not found (404)" + (d.empty() ? std::string() : ": " + d) + ". Check 'model' and 'base_url' in the assistant settings.";
	}
	else if (status == 408) {
		e.kind = LlmError::TIMEOUT;
		e.message = "The provider timed out (408). Try again.";
	}
	else if (status == 413) {
		e.kind = LlmError::BAD_REQUEST;
		e.message = "Request too large (413). Start a new chat to shorten the conversation.";
	}
	else if (status == 429) {
		e.kind = LlmError::RATE_LIMIT;
		e.message = "Rate limited (429). Wait a moment and try again.";
	}
	else if (status >= 500 && status <= 599) {
		e.kind = LlmError::SERVER;
		e.message = "The provider had a server error (" + st + "). Try again later.";
	}
	else {
		e.kind = LlmError::OTHER;
		e.message = "Unexpected HTTP status " + st + (d.empty() ? std::string() : ": " + d);
		if (status >= 300 && status <= 399)
			e.message += ". The server redirected the request; check that 'base_url' is correct (for hosted providers use https://).";
	}
	return e;
}


LlmError httpError(long httpStatus, const std::string& body) {
	json_error_t jerror;
	json_t* rootJ = json_loadb(body.data(), body.size(), 0, &jerror);
	std::string detail = extractDetail(rootJ, body, true);
	if (rootJ)
		json_decref(rootJ);
	return makeHttpError(httpStatus, detail);
}


/** Builds an LlmError from a JSON "error" value found inside a (successful) response. */
static LlmError errorFromErrorValue(json_t* errJ, const std::string& body) {
	// Reuse the extraction logic by wrapping the value like an error response body
	json_t* wrapJ = json_object();
	json_incref(errJ);
	json_object_set_new(wrapJ, "error", errJ);
	std::string detail = extractDetail(wrapJ, body, false);
	json_decref(wrapJ);

	// error.code: HTTP-like int (also accepted as numeric string)
	long code = 0;
	if (errJ && json_is_object(errJ)) {
		json_t* codeJ = json_object_get(errJ, "code");
		if (codeJ && json_is_integer(codeJ))
			code = (long) json_integer_value(codeJ);
		else if (codeJ && json_is_string(codeJ)) {
			char* end = NULL;
			long v = std::strtol(json_string_value(codeJ), &end, 10);
			if (end && *end == '\0' && end != json_string_value(codeJ))
				code = v;
		}
	}
	if (code >= 400 && code <= 599)
		return makeHttpError(code, detail);

	LlmError e;
	e.kind = LlmError::OTHER;
	e.detail = detail;
	e.message = "The provider returned an error" + (detail.empty() ? std::string(".") : ": " + detail);
	return e;
}


static ChatResponse errorResponse(const LlmError& e) {
	ChatResponse r;
	r.ok = false;
	r.error = e;
	return r;
}


static ChatResponse badResponse(const std::string& message, const std::string& detail) {
	LlmError e;
	e.kind = LlmError::BAD_RESPONSE;
	e.message = message;
	e.detail = truncateUtf8(sanitizeUtf8(detail), DETAIL_MAX_BYTES);
	return errorResponse(e);
}


ChatResponse parseResponse(long httpStatus, const std::string& body) {
	if (httpStatus < 200 || httpStatus > 299)
		return errorResponse(httpError(httpStatus, body));

	json_error_t jerror;
	json_t* rootJ = json_loadb(body.data(), body.size(), 0, &jerror);
	if (!rootJ)
		return badResponse("The server returned invalid JSON", body);

	struct Defer {
		json_t* j;
		~Defer() { json_decref(j); }
	} defer = {rootJ};

	if (!json_is_object(rootJ))
		return badResponse("The server returned an unexpected JSON structure", body);

	// Error inside a successful response
	json_t* errJ = json_object_get(rootJ, "error");
	if (errJ && !json_is_null(errJ))
		return errorResponse(errorFromErrorValue(errJ, body));

	json_t* choicesJ = json_object_get(rootJ, "choices");
	if (!choicesJ || !json_is_array(choicesJ) || json_array_size(choicesJ) == 0)
		return badResponse("The response contained no choices", body);
	json_t* choiceJ = json_array_get(choicesJ, 0);
	if (!json_is_object(choiceJ))
		return badResponse("The response contained an invalid choice", body);

	json_t* choiceErrJ = json_object_get(choiceJ, "error");
	if (choiceErrJ && !json_is_null(choiceErrJ))
		return errorResponse(errorFromErrorValue(choiceErrJ, body));

	json_t* msgJ = json_object_get(choiceJ, "message");
	ChatMessage m;
	if (!msgJ || !json_is_object(msgJ))
		return badResponse("The response contained no message", body);
	// The role of a response message is always assistant
	json_t* msgCopyJ = json_deep_copy(msgJ);
	json_object_del(msgCopyJ, "role");
	bool parsed = parseMessageObject(msgCopyJ, &m);
	json_decref(msgCopyJ);
	if (!parsed)
		return badResponse("The response contained an invalid message", body);
	m.role = ChatMessage::ASSISTANT;
	m.toolCallId.clear();
	// OpenAI structured refusal: content is null and "refusal" holds the text. Surface it as the
	// reply so the user doesn't see an empty answer (and no empty message is echoed back).
	if (m.content.empty() && m.toolCalls.empty()) {
		std::string refusal = getString(msgJ, "refusal");
		if (!string::trim(refusal).empty()) {
			m.content = refusal;
			m.contentNull = false;
		}
	}

	ChatResponse r;
	r.ok = true;
	r.message = m;
	r.finishReason = getString(choiceJ, "finish_reason");
	r.model = getString(rootJ, "model");

	json_t* usageJ = json_object_get(rootJ, "usage");
	if (usageJ && json_is_object(usageJ)) {
		getInt(usageJ, "prompt_tokens", &r.usage.promptTokens);
		getInt(usageJ, "completion_tokens", &r.usage.completionTokens);
		getInt(usageJ, "total_tokens", &r.usage.totalTokens);
		json_t* detailsJ = json_object_get(usageJ, "completion_tokens_details");
		if (detailsJ && json_is_object(detailsJ))
			getInt(detailsJ, "reasoning_tokens", &r.usage.reasoningTokens);
		json_t* costJ = json_object_get(usageJ, "cost");
		if (costJ && json_is_number(costJ))
			r.usage.cost = json_number_value(costJ);
	}
	return r;
}


} // namespace assistant
} // namespace rack

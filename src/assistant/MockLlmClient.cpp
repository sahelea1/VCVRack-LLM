#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>
#include <system.hpp>
#include <string.hpp>
#include <logger.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>


namespace rack {
namespace assistant {


static const int LATENCY_MS = 250;
static const int LATENCY_STEP_MS = 20;


// JSON helpers (jansson values are only used locally)

namespace {

struct Json {
	json_t* j = NULL;
	Json() {}
	explicit Json(json_t* j) : j(j) {}
	~Json() {
		if (j)
			json_decref(j);
	}
	Json(const Json&) = delete;
	Json& operator=(const Json&) = delete;
};

} // namespace


static json_t* parseJson(const std::string& text) {
	json_error_t error;
	return json_loadb(text.data(), text.size(), 0, &error);
}


static std::string dump(json_t* j) {
	char* s = json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY);
	if (!s)
		return "{}";
	std::string out = s;
	std::free(s);
	return out;
}


/** Reads an id from a JSON integer / integral real / numeric string. */
static bool readId(json_t* j, int64_t* out) {
	if (!j)
		return false;
	if (json_is_integer(j)) {
		*out = (int64_t) json_integer_value(j);
		return true;
	}
	if (json_is_real(j)) {
		// Out-of-range double to integer conversion is undefined behavior
		double v = json_real_value(j);
		if (!(v > -9.0e18 && v < 9.0e18))
			return false;
		*out = (int64_t) v;
		return true;
	}
	if (json_is_string(j)) {
		char* end = NULL;
		long long v = std::strtoll(json_string_value(j), &end, 10);
		if (end && *end == '\0' && end != json_string_value(j)) {
			*out = v;
			return true;
		}
	}
	return false;
}


static std::string jsonStr(json_t* obj, const char* key) {
	json_t* j = obj ? json_object_get(obj, key) : NULL;
	if (j && json_is_string(j))
		return std::string(json_string_value(j), json_string_length(j));
	return "";
}


// Response builders

static ChatResponse textResponse(const std::string& text, int promptChars) {
	ChatResponse r;
	r.ok = true;
	r.message.role = ChatMessage::ASSISTANT;
	r.message.content = text;
	r.finishReason = "stop";
	r.model = "mock";
	r.usage.promptTokens = promptChars / 4;
	r.usage.completionTokens = (int) text.size() / 4 + 1;
	r.usage.totalTokens = r.usage.promptTokens + r.usage.completionTokens;
	return r;
}


struct CallSpec {
	std::string name;
	std::string args;
};


static ChatResponse toolCallsResponse(int round, const std::vector<CallSpec>& calls, int promptChars) {
	ChatResponse r;
	r.ok = true;
	r.message.role = ChatMessage::ASSISTANT;
	r.message.contentNull = true;
	for (size_t i = 0; i < calls.size(); i++) {
		ToolCall tc;
		tc.id = "mock_" + std::to_string(round) + "_" + std::to_string(i);
		tc.name = calls[i].name;
		tc.arguments = calls[i].args;
		r.message.toolCalls.push_back(tc);
	}
	r.finishReason = "tool_calls";
	r.model = "mock";
	r.usage.promptTokens = promptChars / 4;
	r.usage.completionTokens = 10 * (int) calls.size();
	r.usage.totalTokens = r.usage.promptTokens + r.usage.completionTokens;
	return r;
}


static ChatResponse cancelled() {
	ChatResponse r;
	r.ok = false;
	r.error.kind = LlmError::CANCELLED;
	r.error.message = "Cancelled.";
	return r;
}


/** First `maxChars` code points of s. */
static std::string firstChars(const std::string& s, size_t maxChars) {
	size_t chars = 0;
	size_t i = 0;
	while (i < s.size()) {
		if (((unsigned char) s[i] & 0xC0) != 0x80) {
			if (chars == maxChars)
				break;
			chars++;
		}
		i++;
	}
	return s.substr(0, i);
}


static std::string objectArgs(const std::vector<std::pair<std::string, json_t*>>& fields) {
	json_t* o = json_object();
	for (const auto& f : fields)
		json_object_set_new(o, f.first.c_str(), f.second);
	std::string s = dump(o);
	json_decref(o);
	return s;
}


/** Finds the id of the entry in array `key` of `info` whose name contains one of the needles
(tried in order). Falls back to 0. */
static int64_t findPortId(json_t* info, const char* key, const std::vector<std::string>& needles) {
	json_t* arr = json_object_get(info, key);
	if (!arr || !json_is_array(arr))
		return 0;
	for (const std::string& needle : needles) {
		size_t i;
		json_t* e;
		json_array_foreach(arr, i, e) {
			std::string name = string::lowercase(jsonStr(e, "name"));
			if (name.find(needle) != std::string::npos) {
				int64_t id = (int64_t) i;
				readId(json_object_get(e, "id"), &id);
				return id;
			}
		}
	}
	return 0;
}


/** Mock engine, stateless: everything is derived from the request. */
struct MockLlmClient : LlmClient {
	ClientOptions options;

	MockLlmClient(const ClientOptions& o) : options(o) {}

	ChatResponse complete(const ChatRequest& req, const std::atomic<bool>& cancel) override {
		// Simulated latency, cancel-aware
		for (int t = 0; t < LATENCY_MS; t += LATENCY_STEP_MS) {
			if (cancel.load())
				return cancelled();
			std::this_thread::sleep_for(std::chrono::milliseconds(LATENCY_STEP_MS));
		}
		if (cancel.load())
			return cancelled();

		ChatResponse scripted;
		if (scriptResponse(req, &scripted))
			return scripted;
		return scenarioResponse(req);
	}

	/** Responses from the optional script file. Returns false if there is no usable script. */
	bool scriptResponse(const ChatRequest& req, ChatResponse* out) {
		if (options.mockScriptPath.empty())
			return false;
		try {
			if (!system::isFile(options.mockScriptPath))
				return false;
			std::ifstream f(options.mockScriptPath.c_str(), std::ios::in | std::ios::binary);
			if (!f)
				return false;
			std::stringstream ss;
			ss << f.rdbuf();
			Json root(parseJson(ss.str()));
			json_t* responsesJ = root.j ? json_object_get(root.j, "responses") : NULL;
			if (!responsesJ || !json_is_array(responsesJ) || json_array_size(responsesJ) == 0) {
				WARN("Assistant: mock script %s is invalid, using built-in scenarios", options.mockScriptPath.c_str());
				return false;
			}
			size_t index = 0;
			for (const ChatMessage& m : req.messages)
				if (m.role == ChatMessage::ASSISTANT)
					index++;
			index = std::min(index, json_array_size(responsesJ) - 1);
			*out = parseResponse(200, dump(json_array_get(responsesJ, index)));
			return true;
		}
		catch (std::exception& e) {
			return false;
		}
	}

	ChatResponse scenarioResponse(const ChatRequest& req) {
		// Last USER message and the conversation after it
		int lastUser = -1;
		size_t promptChars = 0;
		for (size_t i = 0; i < req.messages.size(); i++) {
			promptChars += req.messages[i].content.size();
			if (req.messages[i].role == ChatMessage::USER)
				lastUser = (int) i;
		}
		int pc = (int) promptChars;
		std::string userText = lastUser >= 0 ? req.messages[lastUser].content : "";
		std::string text = string::lowercase(userText);

		int round = 0;
		std::vector<const ChatMessage*> tools; // TOOL messages of the last assistant turn
		for (size_t i = (size_t) (lastUser + 1); i < req.messages.size(); i++) {
			const ChatMessage& m = req.messages[i];
			if (m.role == ChatMessage::ASSISTANT) {
				round++;
				tools.clear();
			}
			else if (m.role == ChatMessage::TOOL) {
				tools.push_back(&m);
			}
		}

		// /mock-error <code>
		size_t errPos = text.find("/mock-error");
		if (errPos != std::string::npos) {
			long code = std::strtol(text.c_str() + errPos + std::string("/mock-error").size(), NULL, 10);
			if (code <= 0)
				code = 500;
			ChatResponse r;
			r.ok = false;
			r.error = httpError(code, "{\"error\":{\"message\":\"mock error\"}}");
			return r;
		}

		if (text.find("/mock-badargs") != std::string::npos) {
			if (round == 0) {
				std::vector<CallSpec> calls;
				calls.push_back({"add_module", "{not json"});
				return toolCallsResponse(round, calls, pc);
			}
			return textResponse("The tool reported invalid arguments, as expected.", pc);
		}

		if (text.find("/mock-delete") != std::string::npos) {
			if (round == 0) {
				std::vector<CallSpec> calls;
				calls.push_back({"get_patch", "{}"});
				return toolCallsResponse(round, calls, pc);
			}
			if (round == 1) {
				int64_t id = 0;
				bool found = false;
				if (!tools.empty()) {
					Json info(parseJson(tools[0]->content));
					json_t* modules = info.j ? json_object_get(info.j, "modules") : NULL;
					if (modules && json_is_array(modules) && json_array_size(modules) > 0)
						found = readId(json_object_get(json_array_get(modules, 0), "id"), &id);
				}
				if (!found)
					return textResponse("Mock: there is no module to delete.", pc);
				std::vector<CallSpec> calls;
				calls.push_back({"remove_module", objectArgs({{"module_id", json_integer(id)}})});
				return toolCallsResponse(round, calls, pc);
			}
			// Summary of the removal result
			std::string summary = "Mock: removed the module.";
			if (!tools.empty()) {
				Json result(parseJson(tools[0]->content));
				if (result.j && json_is_object(result.j)) {
					json_t* ok = json_object_get(result.j, "ok");
					if (ok && json_is_false(ok)) {
						std::string err = jsonStr(result.j, "error");
						summary = "Mock: the module was not removed (" + err + ").";
					}
					else {
						int64_t id = 0;
						if (readId(json_object_get(result.j, "removed_module_id"), &id))
							summary = "Mock: removed module " + std::to_string(id) + ".";
					}
				}
			}
			return textResponse(summary, pc);
		}

		if (text.find("/mock-loop") != std::string::npos) {
			std::vector<CallSpec> calls;
			calls.push_back({"get_patch", "{}"});
			return toolCallsResponse(round, calls, pc);
		}

		if (text.find("/mock-build") != std::string::npos || text.find("build") != std::string::npos
			|| text.find("bau") != std::string::npos || text.find("demo") != std::string::npos
			|| text.find("acid") != std::string::npos) {
			return buildResponse(round, tools, pc);
		}

		return textResponse("Mock reply: " + firstChars(userText, 200), pc);
	}

	/** The demo build scenario (VCO -> VCF). */
	ChatResponse buildResponse(int round, const std::vector<const ChatMessage*>& tools, int pc) {
		if (round == 0) {
			std::vector<CallSpec> calls;
			calls.push_back({"get_patch", "{}"});
			calls.push_back({"search_modules", "{\"query\":\"VCO\"}"});
			return toolCallsResponse(round, calls, pc);
		}
		if (round == 1) {
			std::vector<CallSpec> calls;
			calls.push_back({"add_module", "{\"plugin\":\"Fundamental\",\"model\":\"VCO\"}"});
			calls.push_back({"add_module", "{\"plugin\":\"Fundamental\",\"model\":\"VCF\"}"});
			return toolCallsResponse(round, calls, pc);
		}

		// Rounds 2+ depend on the previous tool results (two results: VCO, VCF)
		if (tools.size() < 2)
			return textResponse("Mock: could not continue building, the previous tool results are missing.", pc);
		Json vco(parseJson(tools[0]->content));
		Json vcf(parseJson(tools[1]->content));
		int64_t vcoId = 0, vcfId = 0;
		if (round == 2) {
			std::string err;
			if (!(vco.j && readId(json_object_get(vco.j, "module_id"), &vcoId)))
				err = "VCO: " + (vco.j && jsonStr(vco.j, "error").size() ? jsonStr(vco.j, "error") : std::string("no module_id in the result"));
			else if (!(vcf.j && readId(json_object_get(vcf.j, "module_id"), &vcfId)))
				err = "VCF: " + (vcf.j && jsonStr(vcf.j, "error").size() ? jsonStr(vcf.j, "error") : std::string("no module_id in the result"));
			if (!err.empty())
				return textResponse("Mock: could not add the modules (" + err + ").", pc);
			std::vector<CallSpec> calls;
			calls.push_back({"get_module_info", objectArgs({{"module_id", json_integer(vcoId)}})});
			calls.push_back({"get_module_info", objectArgs({{"module_id", json_integer(vcfId)}})});
			return toolCallsResponse(round, calls, pc);
		}
		if (round == 3) {
			// get_module_info results of both modules
			if (!(vco.j && readId(json_object_get(vco.j, "module_id"), &vcoId)))
				return textResponse("Mock: could not read the VCO module info (no module_id in the result).", pc);
			if (!(vcf.j && readId(json_object_get(vcf.j, "module_id"), &vcfId)))
				return textResponse("Mock: could not read the VCF module info (no module_id in the result).", pc);
			int64_t outId = findPortId(vco.j, "outputs", {"saw"});
			int64_t inId = findPortId(vcf.j, "inputs", {"audio", "in"});
			int64_t paramId = findPortId(vcf.j, "params", {"cutoff", "freq"});
			std::vector<CallSpec> calls;
			calls.push_back({"connect", objectArgs({
				{"from_module", json_integer(vcoId)},
				{"output_id", json_integer(outId)},
				{"to_module", json_integer(vcfId)},
				{"input_id", json_integer(inId)},
			})});
			calls.push_back({"set_param", objectArgs({
				{"module_id", json_integer(vcfId)},
				{"param_id", json_integer(paramId)},
				{"value", json_real(0.4)},
			})});
			return toolCallsResponse(round, calls, pc);
		}
		return textResponse("Mock: built VCO \xE2\x86\x92 VCF. Try turning Cutoff and Resonance.", pc);
	}
};


std::shared_ptr<LlmClient> createMockClient(const ClientOptions& o) {
	return std::make_shared<MockLlmClient>(o);
}


} // namespace assistant
} // namespace rack

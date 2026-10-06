#pragma once
#include <string>
#include <vector>

#include <common.hpp>


namespace rack {
/** Built-in LLM chat assistant */
namespace assistant {


struct ToolCall {
	std::string id;
	std::string name;
	/** Raw JSON text as sent by the model. May be invalid JSON. */
	std::string arguments;
};


struct ChatMessage {
	enum Role {
		SYSTEM,
		USER,
		ASSISTANT,
		TOOL
	};
	Role role = USER;
	std::string content;
	/** Assistant message whose content was null/absent (tool-calls-only). Serialized as null. */
	bool contentNull = false;
	/** ASSISTANT only */
	std::vector<ToolCall> toolCalls;
	/** TOOL only */
	std::string toolCallId;
	/** Display-only reasoning text (never sent back). */
	std::string reasoning;
	/** Raw JSON array text of provider "reasoning_details"; echoed back verbatim on later
	requests when reasoning_param_style == "openrouter". Empty if none. */
	std::string reasoningDetailsJson;
};


struct Usage {
	int promptTokens = -1;
	int completionTokens = -1;
	int totalTokens = -1;
	int reasoningTokens = -1;
	/** OpenRouter "usage.cost" if present */
	double cost = -1.0;
};


struct LlmError {
	enum Kind {
		NONE,
		CANCELLED,
		CONFIG,
		NETWORK,
		TIMEOUT,
		AUTH,
		PAYMENT,
		FORBIDDEN,
		NOT_FOUND,
		RATE_LIMIT,
		SERVER,
		BAD_REQUEST,
		BAD_RESPONSE,
		OTHER
	};
	Kind kind = NONE;
	int httpStatus = 0;
	/** User-facing message incl. hint. Never contains the API key. */
	std::string message;
	/** Provider error text (truncated, may be empty). */
	std::string detail;
};


struct ChatResponse {
	bool ok = false;
	/** role ASSISTANT when ok */
	ChatMessage message;
	std::string finishReason;
	/** Model reported by the provider */
	std::string model;
	Usage usage;
	/** kind != NONE when !ok */
	LlmError error;
};


struct ChatRequest {
	/** System message first */
	std::vector<ChatMessage> messages;
	/** JSON array text of tool definitions (OpenAI format) or empty for no tools. */
	std::string toolsJson;
};


} // namespace assistant
} // namespace rack

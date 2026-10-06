#pragma once
#include <string>

#include <jansson.h>

#include <assistant/Types.hpp>
#include <assistant/Config.hpp>


namespace rack {
namespace assistant {


/** "{base}/chat/completions"; trims trailing '/', doesn't double-append if base already ends with it. */
std::string chatCompletionsUrl(const std::string& baseUrl);
/** Compact JSON body for the request. */
std::string buildRequestBody(const Config& c, const ChatRequest& req);
/** Returns a new reference, OpenAI chat format. includeReasoningDetails: echo reasoning_details. */
json_t* messageToJson(const ChatMessage& m, bool includeReasoningDetails);
/** Inverse of messageToJson (for future persistence). */
bool messageFromJson(json_t* j, ChatMessage* out);
/** Parses a chat.completions response (status 2xx or not). */
ChatResponse parseResponse(long httpStatus, const std::string& body);
/** Friendly error for a non-2xx status; extracts the provider message from the body. */
LlmError httpError(long httpStatus, const std::string& body);
/** UTF-8-safe truncation. If `s` is longer than `maxBytes`, keeps at most `maxBytes` bytes (cut at a
code point boundary) and appends "…" (so the result may be up to 3 bytes longer). */
std::string truncateUtf8(const std::string& s, size_t maxBytes);
/** Replaces invalid UTF-8 sequences with U+FFFD. jansson rejects invalid UTF-8 strings, so every
string that goes into JSON should pass through this (or json_stringn_nocheck after it). */
std::string sanitizeUtf8(const std::string& s);
/** Creates a JSON string value from arbitrary bytes (sanitized, never NULL). */
json_t* jsonString(const std::string& s);


} // namespace assistant
} // namespace rack

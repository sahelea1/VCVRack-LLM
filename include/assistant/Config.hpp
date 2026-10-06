#pragma once
#include <string>
#include <vector>
#include <utility>

#include <jansson.h>

#include <common.hpp>


namespace rack {
namespace assistant {


struct Config {
	std::string baseUrl = "https://openrouter.ai/api/v1";
	std::string model = "openai/gpt-5.6-terra";
	/** off|none|minimal|low|medium|high. "off" sends no reasoning parameter at all (non-reasoning
	models); "none" sends effort "none" (OpenAI reasoning models only accept tools on
	chat/completions with "none"). */
	std::string reasoningEffort = "medium";
	/** openrouter|openai */
	std::string reasoningParamStyle = "openrouter";
	/** <= 0: unset, not sent */
	int maxTokens = 0;
	/** false: not sent */
	bool hasTemperature = false;
	float temperature = 1.f;
	std::vector<std::pair<std::string, std::string>> extraHeaders = {
		{"HTTP-Referer", "https://github.com/sahelea1/vcvrack-llm"},
		{"X-Title", "VCV Rack Assistant"},
	};
	bool confirmDestructive = true;
	/** clamp 1..100 */
	int maxToolRounds = 15;
	/** Total request timeout, clamp 10..1800 */
	double timeoutSec = 180.0;
	/** Optional CA file path for TLS */
	std::string caBundle;
	/** Use MockLlmClient (no network, no cost) */
	bool mock = false;
	/** Only from file; never logged */
	std::string apiKey;
	/** UI, clamp 280..1200 */
	float panelWidth = 400.f;
	/** UI toggle default */
	bool attachSelection = true;
	/** Conversation trimming threshold */
	int maxContextChars = 200000;
};


Config defaultConfig();
/** asset::user("assistant.json") */
std::string configPath();
/** Type-checked parse; invalid values keep defaults and add a warning string. */
Config configFromJson(json_t* rootJ, std::vector<std::string>* warnings = NULL);
/** Returns a new reference. Writes "api_key" only if non-empty. */
json_t* configToJson(const Config& c);
/** Missing file: returns defaults and writes a defaults file (without key). Bad JSON:
returns defaults + warning (file untouched). Never throws. */
Config loadConfig(const std::string& path, std::vector<std::string>* warnings = NULL);
/** Atomic write (path.tmp + rename), chmod 0600 on POSIX. */
bool saveConfig(const Config& c, const std::string& path, std::string* error = NULL);


struct ResolvedKey {
	std::string key;
	/** "RACK_ASSISTANT_API_KEY" | "OPENROUTER_API_KEY" | "assistant.json" | "" (none) */
	std::string source;
};
/** Order: env RACK_ASSISTANT_API_KEY, env OPENROUTER_API_KEY, config.apiKey. Empty env vars are skipped.
Keys are trimmed (surrounding spaces, tabs, CR/LF); whitespace-only values count as empty. */
ResolvedKey resolveApiKey(const Config& c);
/** Float temperature as a double rounded to 6 significant digits (0.7f -> 0.7), for JSON output. */
double temperatureToDouble(float t);
/** "sk-or-v1…abcd" style: first 8 chars + "…" + last 4; keys < 16 chars -> "••••". */
std::string maskKey(const std::string& key);
bool isValidReasoningEffort(const std::string& s);
bool isValidReasoningParamStyle(const std::string& s);


} // namespace assistant
} // namespace rack

#include <assistant/Config.hpp>
#include <assistant/Protocol.hpp>
#include <asset.hpp>
#include <system.hpp>
#include <string.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if !defined ARCH_WIN
	#include <fcntl.h>
	#include <unistd.h>
	#include <sys/stat.h>
#endif


namespace rack {
namespace assistant {


Config defaultConfig() {
	return Config();
}


std::string configPath() {
	return asset::user("assistant.json");
}


bool isValidReasoningEffort(const std::string& s) {
	return s == "off" || s == "none" || s == "minimal" || s == "low" || s == "medium" || s == "high";
}


bool isValidReasoningParamStyle(const std::string& s) {
	return s == "openrouter" || s == "openai";
}


static void warn(std::vector<std::string>* warnings, const std::string& msg) {
	if (warnings)
		warnings->push_back(msg);
}


static bool hasNewline(const std::string& s) {
	return s.find_first_of("\r\n") != std::string::npos;
}


/** Reads an integral number. Accepts JSON integers and integral reals. */
static bool readInt(json_t* j, double* out) {
	if (json_is_integer(j)) {
		*out = (double) json_integer_value(j);
		return true;
	}
	if (json_is_real(j)) {
		double v = json_real_value(j);
		if (std::isfinite(v)) {
			*out = v;
			return true;
		}
	}
	return false;
}


static bool readBool(json_t* rootJ, const char* key, bool* out, std::vector<std::string>* warnings) {
	json_t* j = json_object_get(rootJ, key);
	if (!j)
		return false;
	if (!json_is_boolean(j)) {
		warn(warnings, std::string("'") + key + "' must be true or false; using the default.");
		return false;
	}
	*out = json_is_true(j);
	return true;
}


static bool readString(json_t* rootJ, const char* key, std::string* out, std::vector<std::string>* warnings) {
	json_t* j = json_object_get(rootJ, key);
	if (!j)
		return false;
	if (!json_is_string(j)) {
		warn(warnings, std::string("'") + key + "' must be a string; using the default.");
		return false;
	}
	*out = std::string(json_string_value(j), json_string_length(j));
	return true;
}


/** Reads a number clamped to [lo, hi], warns if it had to be clamped. */
static bool readClamped(json_t* rootJ, const char* key, double lo, double hi, double* out, std::vector<std::string>* warnings) {
	json_t* j = json_object_get(rootJ, key);
	if (!j)
		return false;
	double v = 0.0;
	if (!(json_is_number(j) && readInt(j, &v))) {
		warn(warnings, std::string("'") + key + "' must be a number; using the default.");
		return false;
	}
	if (v < lo || v > hi) {
		warn(warnings, std::string("'") + key + "' is out of range (" + string::f("%g", lo) + ".." + string::f("%g", hi) + "); clamped.");
		v = std::fmin(std::fmax(v, lo), hi);
	}
	*out = v;
	return true;
}


Config configFromJson(json_t* rootJ, std::vector<std::string>* warnings) {
	Config c = defaultConfig();
	if (!rootJ || !json_is_object(rootJ)) {
		warn(warnings, "The config root must be a JSON object; using defaults.");
		return c;
	}

	readString(rootJ, "base_url", &c.baseUrl, warnings);
	readString(rootJ, "model", &c.model, warnings);
	readString(rootJ, "ca_bundle", &c.caBundle, warnings);
	readString(rootJ, "api_key", &c.apiKey, warnings);

	std::string s;
	if (readString(rootJ, "reasoning_effort", &s, warnings)) {
		if (isValidReasoningEffort(s))
			c.reasoningEffort = s;
		else
			warn(warnings, "'reasoning_effort' must be one of off, none, minimal, low, medium, high; using the default.");
	}
	if (readString(rootJ, "reasoning_param_style", &s, warnings)) {
		if (isValidReasoningParamStyle(s))
			c.reasoningParamStyle = s;
		else
			warn(warnings, "'reasoning_param_style' must be 'openrouter' or 'openai'; using the default.");
	}

	// max_tokens: null / missing / 0 / "" -> unset
	json_t* maxTokensJ = json_object_get(rootJ, "max_tokens");
	if (maxTokensJ && !json_is_null(maxTokensJ)) {
		double v = 0.0;
		if (json_is_string(maxTokensJ) && std::string(json_string_value(maxTokensJ)).empty()) {
			c.maxTokens = 0;
		}
		else if (json_is_number(maxTokensJ) && readInt(maxTokensJ, &v)) {
			if (v <= 0.0)
				c.maxTokens = 0;
			else
				c.maxTokens = (int) std::fmin(v, 10000000.0);
		}
		else {
			warn(warnings, "'max_tokens' must be a number or null; leaving it unset.");
		}
	}

	// temperature: null / missing / "" -> unset
	json_t* temperatureJ = json_object_get(rootJ, "temperature");
	if (temperatureJ && !json_is_null(temperatureJ)) {
		double v = 0.0;
		if (json_is_string(temperatureJ) && std::string(json_string_value(temperatureJ)).empty()) {
			c.hasTemperature = false;
		}
		else if (json_is_number(temperatureJ) && readInt(temperatureJ, &v)) {
			if (v < 0.0 || v > 2.0) {
				warn(warnings, "'temperature' must be between 0 and 2; leaving it unset.");
			}
			else {
				c.hasTemperature = true;
				c.temperature = (float) v;
			}
		}
		else {
			warn(warnings, "'temperature' must be a number or null; leaving it unset.");
		}
	}

	// extra_headers: object of string -> string, replaces the defaults when present
	json_t* headersJ = json_object_get(rootJ, "extra_headers");
	if (headersJ && !json_is_null(headersJ)) {
		if (!json_is_object(headersJ)) {
			warn(warnings, "'extra_headers' must be an object of strings; using the defaults.");
		}
		else {
			c.extraHeaders.clear();
			const char* name;
			json_t* valueJ;
			json_object_foreach(headersJ, name, valueJ) {
				if (!json_is_string(valueJ)) {
					warn(warnings, std::string("Extra header '") + name + "' must have a string value; dropped.");
					continue;
				}
				std::string value(json_string_value(valueJ), json_string_length(valueJ));
				std::string headerName = name;
				if (headerName.empty() || headerName.find(':') != std::string::npos || hasNewline(headerName) || hasNewline(value)) {
					warn(warnings, "An extra header with an invalid name or a CR/LF character was dropped.");
					continue;
				}
				c.extraHeaders.push_back(std::make_pair(headerName, value));
			}
		}
	}

	readBool(rootJ, "confirm_destructive", &c.confirmDestructive, warnings);
	readBool(rootJ, "mock", &c.mock, warnings);
	readBool(rootJ, "attach_selection", &c.attachSelection, warnings);

	double v = 0.0;
	if (readClamped(rootJ, "max_tool_rounds", 1, 100, &v, warnings))
		c.maxToolRounds = (int) v;
	if (readClamped(rootJ, "timeout_sec", 10, 1800, &v, warnings))
		c.timeoutSec = v;
	if (readClamped(rootJ, "panel_width", 280, 1200, &v, warnings))
		c.panelWidth = (float) v;
	if (readClamped(rootJ, "max_context_chars", 1000, 100000000, &v, warnings))
		c.maxContextChars = (int) v;

	return c;
}


json_t* configToJson(const Config& c) {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "base_url", jsonString(c.baseUrl));
	json_object_set_new(rootJ, "model", jsonString(c.model));
	json_object_set_new(rootJ, "reasoning_effort", jsonString(c.reasoningEffort));
	json_object_set_new(rootJ, "reasoning_param_style", jsonString(c.reasoningParamStyle));
	json_object_set_new(rootJ, "max_tokens", c.maxTokens > 0 ? json_integer(c.maxTokens) : json_null());
	json_object_set_new(rootJ, "temperature", c.hasTemperature ? json_real(temperatureToDouble(c.temperature)) : json_null());
	json_t* headersJ = json_object();
	for (const auto& h : c.extraHeaders)
		json_object_set_new(headersJ, h.first.c_str(), jsonString(h.second));
	json_object_set_new(rootJ, "extra_headers", headersJ);
	json_object_set_new(rootJ, "confirm_destructive", json_boolean(c.confirmDestructive));
	json_object_set_new(rootJ, "max_tool_rounds", json_integer(c.maxToolRounds));
	json_object_set_new(rootJ, "timeout_sec", json_real(c.timeoutSec));
	json_object_set_new(rootJ, "ca_bundle", jsonString(c.caBundle));
	json_object_set_new(rootJ, "mock", json_boolean(c.mock));
	if (!c.apiKey.empty())
		json_object_set_new(rootJ, "api_key", jsonString(c.apiKey));
	json_object_set_new(rootJ, "panel_width", json_real(c.panelWidth));
	json_object_set_new(rootJ, "attach_selection", json_boolean(c.attachSelection));
	json_object_set_new(rootJ, "max_context_chars", json_integer(c.maxContextChars));
	return rootJ;
}


Config loadConfig(const std::string& path, std::vector<std::string>* warnings) {
	Config c = defaultConfig();
	try {
		if (!system::exists(path)) {
			// First run: write a defaults file (without key)
			std::string error;
			if (!saveConfig(c, path, &error))
				warn(warnings, "Could not write the default config " + path + ": " + error);
			return c;
		}

		std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
		if (!f) {
			warn(warnings, "Could not read " + path + "; using defaults.");
			return c;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		std::string text = ss.str();

		json_error_t error;
		json_t* rootJ = json_loadb(text.data(), text.size(), 0, &error);
		if (!rootJ) {
			warn(warnings, "Could not parse " + path + " (line " + std::to_string(error.line) + "): " + error.text + ". Using defaults.");
			return c;
		}
		c = configFromJson(rootJ, warnings);
		json_decref(rootJ);
	}
	catch (std::exception& e) {
		warn(warnings, std::string("Error while loading ") + path + ": " + e.what());
		return defaultConfig();
	}
	return c;
}


bool saveConfig(const Config& c, const std::string& path, std::string* error) {
	json_t* rootJ = configToJson(c);
	char* dump = json_dumps(rootJ, JSON_INDENT(2) | JSON_PRESERVE_ORDER | JSON_REAL_PRECISION(15));
	json_decref(rootJ);
	if (!dump) {
		if (error)
			*error = "Could not serialize the config.";
		return false;
	}
	std::string text = dump;
	std::free(dump);
	text += "\n";

	std::string tmpPath = path + ".tmp";
	try {
		std::string dir = system::getDirectory(path);
		if (!dir.empty() && !system::isDirectory(dir))
			system::createDirectories(dir);
	}
	catch (std::exception& e) {
		// Fall through, opening the file reports the real problem
	}

#if defined ARCH_WIN
	{
		std::ofstream f(tmpPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
		if (!f) {
			if (error)
				*error = "Could not open " + tmpPath + " for writing.";
			return false;
		}
		f << text;
		f.close();
		if (!f) {
			if (error)
				*error = "Could not write " + tmpPath + ".";
			std::remove(tmpPath.c_str());
			return false;
		}
	}
	try {
		if (!system::rename(tmpPath, path)) {
			if (error)
				*error = "Could not rename " + tmpPath + " to " + path + ".";
			system::remove(tmpPath);
			return false;
		}
	}
	catch (std::exception& e) {
		if (error)
			*error = e.what();
		return false;
	}
	return true;
#else
	// The file may hold an API key: create it with 0600 from the start.
	int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		if (error)
			*error = "Could not open " + tmpPath + " for writing: " + std::strerror(errno);
		return false;
	}
	bool ok = true;
	size_t written = 0;
	while (written < text.size()) {
		ssize_t n = ::write(fd, text.data() + written, text.size() - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ok = false;
			break;
		}
		written += (size_t) n;
	}
	if (ok && ::fchmod(fd, 0600) != 0)
		ok = false;
	if (ok && ::fsync(fd) != 0)
		ok = false;
	if (::close(fd) != 0)
		ok = false;
	if (!ok) {
		if (error)
			*error = "Could not write " + tmpPath + ": " + std::strerror(errno);
		::unlink(tmpPath.c_str());
		return false;
	}
	if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
		if (error)
			*error = "Could not rename " + tmpPath + " to " + path + ": " + std::strerror(errno);
		::unlink(tmpPath.c_str());
		return false;
	}
	// Existing destination keeps the mode of the replaced inode only on some systems; the temp file already had 0600.
	::chmod(path.c_str(), 0600);
	return true;
#endif
}


double temperatureToDouble(float t) {
	// Round to 6 significant digits so that 0.7f becomes 0.7 and not 0.699999988079071
	return std::strtod(string::f("%.6g", (double) t).c_str(), NULL);
}


ResolvedKey resolveApiKey(const Config& c) {
	ResolvedKey r;
	// Keys pasted into a file or exported from `echo`/`cat` often carry a trailing newline or
	// spaces; trim them. Whitespace-only values count as empty.
	const char* names[] = {"RACK_ASSISTANT_API_KEY", "OPENROUTER_API_KEY"};
	for (const char* name : names) {
		const char* v = std::getenv(name);
		if (v) {
			std::string key = string::trim(v);
			if (!key.empty()) {
				r.key = key;
				r.source = name;
				return r;
			}
		}
	}
	std::string key = string::trim(c.apiKey);
	if (!key.empty()) {
		r.key = key;
		r.source = "assistant.json";
	}
	return r;
}


std::string maskKey(const std::string& key) {
	if (key.size() < 16)
		return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
	return key.substr(0, 8) + "\xE2\x80\xA6" + key.substr(key.size() - 4);
}


} // namespace assistant
} // namespace rack

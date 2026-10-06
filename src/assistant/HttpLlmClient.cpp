#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>
#include <asset.hpp>
#include <settings.hpp>
#include <system.hpp>
#include <string.hpp>
#include <logger.hpp>

#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <exception>

#define CURL_STATICLIB
#include <curl/curl.h>


namespace rack {
namespace assistant {


/** Upper bound for a response body, protects against runaway servers. */
static const size_t MAX_RESPONSE_BYTES = 64 * 1024 * 1024;
/** CURLOPT_CONNECTTIMEOUT in seconds */
static const long CONNECT_TIMEOUT_SEC = 30;


ClientOptions makeClientOptions(const Config& c) {
	// UI thread only: reads settings:: and asset paths
	ClientOptions o;
	o.config = c;
	o.apiKey = resolveApiKey(c).key;
	o.verifyTls = settings::verifyHttpsCerts;
	if (!c.caBundle.empty()) {
		o.caBundlePath = c.caBundle;
	}
	else {
		const char* env = std::getenv("RACK_ASSISTANT_CA_BUNDLE");
		if (env && env[0])
			o.caBundlePath = env;
		else
			o.caBundlePath = asset::system("cacert.pem");
	}
	o.userAgent = "VCV Rack " + APP_VERSION + " Assistant";
	o.mockScriptPath = asset::user("assistant-mock.json");
	return o;
}


std::shared_ptr<LlmClient> createClient(const ClientOptions& o) {
	if (o.config.mock || string::startsWith(string::lowercase(string::trim(o.config.baseUrl)), "mock:"))
		return createMockClient(o);
	return createHttpClient(o);
}


/** Returns the host part of a URL ("" if none). Handles userinfo, ports and [ipv6]. */
static std::string urlHost(const std::string& url) {
	size_t start = url.find("://");
	start = (start == std::string::npos) ? 0 : start + 3;
	size_t end = url.find_first_of("/?#", start);
	std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	if (!authority.empty() && authority[0] == '[') {
		size_t close = authority.find(']');
		if (close != std::string::npos)
			return authority.substr(0, close + 1);
		return authority;
	}
	size_t colon = authority.find(':');
	if (colon != std::string::npos)
		authority = authority.substr(0, colon);
	return authority;
}


static bool isLocalHost(const std::string& host) {
	std::string h = string::lowercase(host);
	return h == "localhost" || h == "127.0.0.1" || h == "[::1]";
}


/** URL without userinfo and query, safe for logs. */
static std::string urlForLog(const std::string& url) {
	size_t start = url.find("://");
	start = (start == std::string::npos) ? 0 : start + 3;
	size_t end = url.find_first_of("/?#", start);
	std::string scheme = url.substr(0, start);
	std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	std::string rest;
	if (end != std::string::npos) {
		rest = url.substr(end);
		size_t q = rest.find_first_of("?#");
		if (q != std::string::npos)
			rest = rest.substr(0, q);
	}
	size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	return scheme + authority + rest;
}


static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
	// Exceptions must not propagate through libcurl's C frames
	try {
		std::string* body = (std::string*) userdata;
		size_t n = size * nmemb;
		if (body->size() + n > MAX_RESPONSE_BYTES)
			return 0; // aborts the transfer (CURLE_WRITE_ERROR)
		body->append(ptr, n);
		return n;
	}
	catch (...) {
		return 0;
	}
}


static int xferInfoCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
	const std::atomic<bool>* cancel = (const std::atomic<bool>*) userdata;
	return cancel->load() ? 1 : 0;
}


static ChatResponse makeError(LlmError::Kind kind, const std::string& message, const std::string& detail = "") {
	ChatResponse r;
	r.ok = false;
	r.error.kind = kind;
	r.error.message = message;
	r.error.detail = detail;
	return r;
}


static bool isTlsError(CURLcode code) {
	switch (code) {
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_PEER_FAILED_VERIFICATION:
		case CURLE_SSL_ENGINE_NOTFOUND:
		case CURLE_SSL_ENGINE_SETFAILED:
		case CURLE_SSL_CERTPROBLEM:
		case CURLE_SSL_CIPHER:
		case CURLE_SSL_CACERT_BADFILE:
		case CURLE_SSL_CRL_BADFILE:
		case CURLE_SSL_ISSUER_ERROR:
		case CURLE_SSL_ENGINE_INITFAILED:
		case CURLE_SSL_INVALIDCERTSTATUS:
			return true;
		default:
			return false;
	}
}


struct HttpLlmClient : LlmClient {
	ClientOptions options;

	HttpLlmClient(const ClientOptions& o) : options(o) {}

	ChatResponse complete(const ChatRequest& req, const std::atomic<bool>& cancel) override {
		// Never let an exception (e.g. std::bad_alloc) escape the worker thread
		try {
			return completeImpl(req, cancel);
		}
		catch (std::exception& e) {
			return makeError(LlmError::OTHER, std::string("Unexpected error while contacting the provider: ") + e.what());
		}
		catch (...) {
			return makeError(LlmError::OTHER, "Unexpected error while contacting the provider.");
		}
	}

	ChatResponse completeImpl(const ChatRequest& req, const std::atomic<bool>& cancel) {
		if (cancel.load())
			return makeError(LlmError::CANCELLED, "Cancelled.");

		const Config& c = options.config;
		if (string::trim(c.baseUrl).empty())
			return makeError(LlmError::CONFIG, "No base URL configured. Set 'base_url' in the assistant settings.");

		std::string url = chatCompletionsUrl(c.baseUrl);
		std::string host = urlHost(url);
		if (options.apiKey.empty() && !isLocalHost(host))
			return makeError(LlmError::CONFIG, "No API key configured. Set RACK_ASSISTANT_API_KEY or OPENROUTER_API_KEY, or enter a key in the assistant settings.");

		// Never send a key in cleartext to a remote host. Plain http is only accepted for local servers.
		if (!options.apiKey.empty() && string::startsWith(string::lowercase(url), "http://") && !isLocalHost(host))
			return makeError(LlmError::CONFIG, "Refusing to send the API key over plain http:// to a non-local host. Use an https:// base URL.");

		// A key with line breaks or other control characters can't be sent in a header. Report it
		// instead of silently sending the request without authentication.
		for (char ch : options.apiKey) {
			unsigned char u = (unsigned char) ch;
			if (u < 0x20 || u == 0x7F)
				return makeError(LlmError::CONFIG, "The API key contains line breaks or control characters. Check the key in RACK_ASSISTANT_API_KEY / OPENROUTER_API_KEY or the assistant settings.");
		}

		std::string body = buildRequestBody(c, req);

		CURL* curl = curl_easy_init();
		if (!curl)
			return makeError(LlmError::NETWORK, "Could not initialize the HTTP client.");

		// Frees the handle and the header list on every exit path, including exceptions.
		// Header values are copied by curl_slist_append.
		struct curl_slist* headers = NULL;
		struct Cleanup {
			CURL* curl;
			struct curl_slist** headers;
			~Cleanup() {
				curl_easy_cleanup(curl);
				curl_slist_free_all(*headers);
			}
		} cleanup = {curl, &headers};
		headers = curl_slist_append(headers, "Content-Type: application/json");
		headers = curl_slist_append(headers, "Accept: application/json");
		// Don't wait for "100 Continue" on large bodies
		headers = curl_slist_append(headers, "Expect:");
		if (!options.apiKey.empty())
			headers = curl_slist_append(headers, ("Authorization: Bearer " + options.apiKey).c_str());
		for (const auto& h : c.extraHeaders) {
			if (h.first.empty() || h.first.find_first_of(":\r\n") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos)
				continue;
			headers = curl_slist_append(headers, (h.first + ": " + h.second).c_str());
		}

		std::string response;
		char errorBuffer[CURL_ERROR_SIZE];
		errorBuffer[0] = '\0';

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) body.size());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		if (!options.userAgent.empty())
			curl_easy_setopt(curl, CURLOPT_USERAGENT, options.userAgent.c_str());
		// Required when running off the main thread
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SEC);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long) (c.timeoutSec * 1000.0));
		if (!options.caBundlePath.empty())
			curl_easy_setopt(curl, CURLOPT_CAINFO, options.caBundlePath.c_str());
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, options.verifyTls ? 1L : 0L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfoCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*) &cancel);

		INFO("Assistant: POST %s (model %s, %zu bytes)", urlForLog(url).c_str(), c.model.c_str(), body.size());
		auto startTime = std::chrono::steady_clock::now();
		CURLcode res = curl_easy_perform(curl);
		double duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();

		long status = 0;
		if (res == CURLE_OK)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		std::string curlText = errorBuffer[0] ? errorBuffer : curl_easy_strerror(res);

		if (res != CURLE_OK) {
			INFO("Assistant: request failed (%s), %.1f s", curl_easy_strerror(res), duration);
			if (res == CURLE_ABORTED_BY_CALLBACK || cancel.load())
				return makeError(LlmError::CANCELLED, "Cancelled.");
			if (res == CURLE_OPERATION_TIMEDOUT) {
				// The connect timeout also yields CURLE_OPERATION_TIMEDOUT. The total timeout only fires
				// after timeoutSec, so a shorter wait means that the connection could not be established.
				if (c.timeoutSec > (double) CONNECT_TIMEOUT_SEC && duration < c.timeoutSec - 1.0)
					return makeError(LlmError::TIMEOUT, string::f("The request timed out after %ld s while connecting to ", CONNECT_TIMEOUT_SEC) + host + ".", curlText);
				return makeError(LlmError::TIMEOUT, string::f("The request timed out after %.0f s.", c.timeoutSec), curlText);
			}
			if (res == CURLE_WRITE_ERROR)
				return makeError(LlmError::BAD_RESPONSE, "The response from the server was too large.", curlText);
			if (isTlsError(res))
				return makeError(LlmError::NETWORK, "TLS error: " + curlText + ". If you are behind a proxy, set 'ca_bundle'.", curlText);
			if (res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_COULDNT_RESOLVE_PROXY || res == CURLE_COULDNT_CONNECT)
				return makeError(LlmError::NETWORK, "Could not connect to " + host + ": " + curlText + ".", curlText);
			return makeError(LlmError::NETWORK, "Network error: " + curlText + ".", curlText);
		}

		INFO("Assistant: HTTP %ld, %zu bytes, %.1f s", status, response.size(), duration);
		return parseResponse(status, response);
	}
};


std::shared_ptr<LlmClient> createHttpClient(const ClientOptions& o) {
	return std::make_shared<HttpLlmClient>(o);
}


} // namespace assistant
} // namespace rack

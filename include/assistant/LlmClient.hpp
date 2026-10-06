#pragma once
#include <atomic>
#include <memory>
#include <string>

#include <assistant/Types.hpp>
#include <assistant/Config.hpp>


namespace rack {
namespace assistant {


struct LlmClient {
	virtual ~LlmClient() {}
	/** Blocking; runs on a worker thread. Must not touch APP/widgets/settings.
	Returns within ~1 s after `cancel` becomes true with error.kind == CANCELLED. */
	virtual ChatResponse complete(const ChatRequest& req, const std::atomic<bool>& cancel) = 0;
	// Phase 2 (streaming): add a virtual completeStream(req, cancel, onDelta) whose default
	// implementation calls complete(). Do not implement now.
};


/** Everything a client needs, resolved on the UI thread (settings/asset access). */
struct ClientOptions {
	Config config;
	/** Resolved key (may be empty) */
	std::string apiKey;
	/** Snapshot of settings::verifyHttpsCerts */
	bool verifyTls = true;
	/** config.caBundle, else env RACK_ASSISTANT_CA_BUNDLE, else asset::system("cacert.pem") */
	std::string caBundlePath;
	/** "VCV Rack <version> Assistant" */
	std::string userAgent;
	/** asset::user("assistant-mock.json") */
	std::string mockScriptPath;
};

/** UI thread only. */
ClientOptions makeClientOptions(const Config& c);
/** Mock if options.config.mock or base_url starts with "mock:"; else HTTP. */
std::shared_ptr<LlmClient> createClient(const ClientOptions& o);
std::shared_ptr<LlmClient> createHttpClient(const ClientOptions& o);
std::shared_ptr<LlmClient> createMockClient(const ClientOptions& o);


} // namespace assistant
} // namespace rack

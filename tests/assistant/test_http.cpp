// Tests HttpLlmClient against a tiny in-process HTTP server on 127.0.0.1 (no external network).
#include "test.hpp"

#include <assistant/LlmClient.hpp>
#include <assistant/Protocol.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>


using namespace rack;
using namespace rack::assistant;


namespace {

struct FakeServer {
	int listenFd = -1;
	int port = 0;
	std::thread thread;
	std::atomic<bool> stop;
	std::mutex mutex;
	std::string request;      // everything received (headers + body)
	std::string responseBody;
	int status = 200;
	/** Time to wait before answering (checked in 20 ms steps against `stop`) */
	int delayMs = 0;
	std::atomic<int> connections;

	FakeServer() : stop(false), connections(0) {}

	bool start() {
		listenFd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenFd < 0)
			return false;
		int one = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(listenFd, (sockaddr*) &addr, sizeof(addr)) != 0)
			return false;
		socklen_t len = sizeof(addr);
		getsockname(listenFd, (sockaddr*) &addr, &len);
		port = ntohs(addr.sin_port);
		if (listen(listenFd, 4) != 0)
			return false;
		thread = std::thread([this]() { run(); });
		return true;
	}

	~FakeServer() {
		stop = true;
		if (thread.joinable())
			thread.join();
		if (listenFd >= 0)
			close(listenFd);
	}

	std::string baseUrl() const {
		return "http://127.0.0.1:" + std::to_string(port) + "/v1";
	}

	std::string getRequest() {
		std::lock_guard<std::mutex> lock(mutex);
		return request;
	}

	void run() {
		while (!stop) {
			pollfd pfd = {listenFd, POLLIN, 0};
			if (poll(&pfd, 1, 20) <= 0)
				continue;
			int fd = accept(listenFd, NULL, NULL);
			if (fd < 0)
				continue;
			connections++;
			handle(fd);
			close(fd);
		}
	}

	void handle(int fd) {
		std::string data;
		size_t headerEnd = std::string::npos;
		size_t contentLength = 0;
		while (!stop) {
			pollfd pfd = {fd, POLLIN, 0};
			if (poll(&pfd, 1, 20) <= 0)
				continue;
			char buf[4096];
			ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n <= 0)
				return;
			data.append(buf, (size_t) n);
			if (headerEnd == std::string::npos) {
				headerEnd = data.find("\r\n\r\n");
				if (headerEnd != std::string::npos) {
					std::string lower;
					for (char ch : data.substr(0, headerEnd))
						lower += (char) tolower((unsigned char) ch);
					size_t cl = lower.find("content-length:");
					if (cl != std::string::npos)
						contentLength = (size_t) atol(lower.c_str() + cl + 15);
				}
			}
			if (headerEnd != std::string::npos && data.size() >= headerEnd + 4 + contentLength)
				break;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			request = data;
		}
		for (int waited = 0; waited < delayMs && !stop; waited += 20)
			usleep(20 * 1000);
		if (stop)
			return;
		std::string body;
		int st;
		{
			std::lock_guard<std::mutex> lock(mutex);
			body = responseBody;
			st = status;
		}
		std::string resp = "HTTP/1.1 " + std::to_string(st) + " X\r\nContent-Type: application/json\r\nContent-Length: "
			+ std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
	}
};


const char* OK_BODY = "{\"model\":\"fake-model\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"pong\"}}],"
	"\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1,\"total_tokens\":4}}";


ChatRequest pingRequest() {
	ChatRequest req;
	ChatMessage u;
	u.role = ChatMessage::USER;
	u.content = "ping";
	req.messages.push_back(u);
	return req;
}


ClientOptions optionsFor(const FakeServer& server, const std::string& key) {
	ClientOptions o;
	o.config.baseUrl = server.baseUrl();
	o.config.model = "test-model";
	o.apiKey = key;
	o.userAgent = "VCV Rack test Assistant";
	o.caBundlePath = "";
	return o;
}


/** Local connections must never go through a proxy configured in the environment. */
struct NoProxyGuard {
	std::string old;
	bool had;
	NoProxyGuard() {
		const char* v = getenv("NO_PROXY");
		had = (v != NULL);
		if (v)
			old = v;
		setenv("NO_PROXY", "*", 1);
		const char* v2 = getenv("no_proxy");
		had2 = (v2 != NULL);
		if (v2)
			old2 = v2;
		setenv("no_proxy", "*", 1);
	}
	~NoProxyGuard() {
		if (had)
			setenv("NO_PROXY", old.c_str(), 1);
		else
			unsetenv("NO_PROXY");
		if (had2)
			setenv("no_proxy", old2.c_str(), 1);
		else
			unsetenv("no_proxy");
	}
	std::string old2;
	bool had2;
};

} // namespace


TEST(http_success_request_shape) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = OK_BODY;

	ClientOptions o = optionsFor(server, "test-key-not-real");
	o.config.maxTokens = 55;
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	REQUIRE(r.ok);
	CHECK_EQ(r.message.content, "pong");
	CHECK_EQ(r.model, "fake-model");
	CHECK_EQ(r.usage.totalTokens, 4);

	std::string req = server.getRequest();
	CHECK(req.find("POST /v1/chat/completions HTTP/1.1") == 0);
	CHECK(req.find("Authorization: Bearer test-key-not-real\r\n") != std::string::npos);
	CHECK(req.find("Content-Type: application/json\r\n") != std::string::npos);
	CHECK(req.find("Accept: application/json\r\n") != std::string::npos);
	CHECK(req.find("User-Agent: VCV Rack test Assistant\r\n") != std::string::npos);
	CHECK(req.find("HTTP-Referer: https://github.com/sahelea1/vcvrack-llm\r\n") != std::string::npos);
	CHECK(req.find("X-Title: VCV Rack Assistant\r\n") != std::string::npos);
	size_t bodyPos = req.find("\r\n\r\n");
	REQUIRE(bodyPos != std::string::npos);
	std::string body = req.substr(bodyPos + 4);
	CHECK(body.find("\"model\":\"test-model\"") != std::string::npos);
	CHECK(body.find("\"max_tokens\":55") != std::string::npos);
	CHECK(body.find("\"content\":\"ping\"") != std::string::npos);
	// The key never goes into the body
	CHECK(body.find("test-key-not-real") == std::string::npos);
}

TEST(http_local_server_without_key_sends_no_authorization) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = OK_BODY;
	ClientOptions o = optionsFor(server, "");
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	REQUIRE(r.ok);
	CHECK(server.getRequest().find("Authorization") == std::string::npos);
}

TEST(http_error_statuses_are_mapped) {
	NoProxyGuard noProxy;
	struct Row {
		int status;
		LlmError::Kind kind;
	};
	const Row rows[] = {
		{401, LlmError::AUTH},
		{402, LlmError::PAYMENT},
		{404, LlmError::NOT_FOUND},
		{429, LlmError::RATE_LIMIT},
		{503, LlmError::SERVER},
	};
	for (const Row& row : rows) {
		FakeServer server;
		REQUIRE(server.start());
		server.status = row.status;
		server.responseBody = "{\"error\":{\"message\":\"nope\"}}";
		ClientOptions o = optionsFor(server, "k");
		std::atomic<bool> cancel(false);
		ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
		CHECK(!r.ok);
		CHECK_EQ((int) r.error.kind, (int) row.kind);
		CHECK_EQ(r.error.httpStatus, row.status);
	}
}

TEST(http_invalid_json_body) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = "<html>not json</html>";
	ClientOptions o = optionsFor(server, "k");
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::BAD_RESPONSE);
}

TEST(http_cancel_aborts_quickly) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = OK_BODY;
	server.delayMs = 10000;
	ClientOptions o = optionsFor(server, "k");
	std::atomic<bool> cancel(false);
	std::thread canceller([&cancel]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		cancel = true;
	});
	auto t0 = std::chrono::steady_clock::now();
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	canceller.join();
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CANCELLED);
	CHECK_EQ(r.error.message, "Cancelled.");
	CHECK(sec < 2.5);
}

TEST(http_cancelled_before_start_does_not_connect) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	ClientOptions o = optionsFor(server, "k");
	std::atomic<bool> cancel(true);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CANCELLED);
	CHECK_EQ(server.connections.load(), 0);
}

TEST(http_timeout) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = OK_BODY;
	server.delayMs = 10000;
	ClientOptions o = optionsFor(server, "k");
	o.config.timeoutSec = 1.0;   // below the config clamp, only possible by setting the field directly
	std::atomic<bool> cancel(false);
	auto t0 = std::chrono::steady_clock::now();
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::TIMEOUT);
	CHECK_EQ(r.error.message, "The request timed out after 1 s.");
	CHECK(sec >= 0.8 && sec < 4.0);
}

TEST(http_connection_refused) {
	NoProxyGuard noProxy;
	// Find a free port by binding and closing again
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(bind(fd, (sockaddr*) &addr, sizeof(addr)) == 0);
	socklen_t len = sizeof(addr);
	getsockname(fd, (sockaddr*) &addr, &len);
	int port = ntohs(addr.sin_port);
	close(fd);

	ClientOptions o;
	o.config.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::NETWORK);
	CHECK(r.error.message.find("Could not connect to 127.0.0.1") == 0);
}

TEST(http_no_key_for_remote_host_is_config_error) {
	ClientOptions o;
	o.config.baseUrl = "https://example.invalid/v1";
	o.apiKey = "";
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CONFIG);
	CHECK_EQ(r.error.message, "No API key configured. Set RACK_ASSISTANT_API_KEY or OPENROUTER_API_KEY, or enter a key in the assistant settings.");
	// userinfo in the URL must not fool the localhost check
	o.config.baseUrl = "https://localhost@example.invalid/v1";
	ChatResponse s = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK_EQ((int) s.error.kind, (int) LlmError::CONFIG);
}

TEST(http_key_with_control_characters_is_config_error) {
	NoProxyGuard noProxy;
	FakeServer server;
	REQUIRE(server.start());
	server.responseBody = OK_BODY;
	std::atomic<bool> cancel(false);
	const char* keys[] = {"abc\ndef", "abc\r\ndef", "abc\tdef", "abc\x01" "def"};
	for (const char* key : keys) {
		ClientOptions o = optionsFor(server, key);
		ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
		CHECK(!r.ok);
		CHECK_EQ((int) r.error.kind, (int) LlmError::CONFIG);
		CHECK(r.error.message.find("API key") != std::string::npos);
		CHECK(r.error.message.find("abc") == std::string::npos);
	}
	// Nothing was sent
	CHECK_EQ(server.connections.load(), 0);
}

TEST(http_empty_base_url_is_config_error) {
	ClientOptions o;
	o.config.baseUrl = "  ";
	o.apiKey = "k";
	std::atomic<bool> cancel(false);
	ChatResponse r = createHttpClient(o)->complete(pingRequest(), cancel);
	CHECK(!r.ok);
	CHECK_EQ((int) r.error.kind, (int) LlmError::CONFIG);
}

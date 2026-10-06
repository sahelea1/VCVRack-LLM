#include "test.hpp"

#include <asset.hpp>
#include <logger.hpp>
#include <network.hpp>
#include <settings.hpp>
#include <system.hpp>
#include <random.hpp>
#include <string.hpp>

#include <cstdlib>
#include <cstring>
#include <unistd.h>


bool liveMode = false;


int main(int argc, char* argv[]) {
	using namespace rack;
	const char* filter = NULL;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--live") == 0)
			liveMode = true;
		else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
			filter = argv[++i];
		else {
			std::fprintf(stderr, "Usage: %s [--live] [--filter <substring>]\n", argv[0]);
			return 2;
		}
	}

	// Dev mode: system dir is the current directory (cacert.pem, res/). The user dir is a
	// throw-away temp dir so the tests never touch the real assistant.json.
	settings::devMode = true;
	settings::headless = true;
	system::init();
	char tmpl[] = "/tmp/rack-assistant-test-XXXXXX";
	char* userDir = mkdtemp(tmpl);
	if (!userDir) {
		std::fprintf(stderr, "Could not create a temp user dir\n");
		return 2;
	}
	asset::userDir = userDir;
	asset::init();
	logger::init();
	random::init();
	string::init();
	network::init();

	int code = test::runAll(liveMode, filter);

	network::destroy();
	logger::destroy();
	// Best effort cleanup of the temp user dir
	system::removeRecursively(userDir);
	return code;
}

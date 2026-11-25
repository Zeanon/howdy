// Prints what the module reads from each argument as a timeout (or with --delay as a delay), in seconds, and
// whether it warns about it.
// Usage: timeout_test [--delay] <value>...
#include <cstdio>
#include <cstring>
#include <string>
#include <syslog.h>
#include <unistd.h>

#include "../../src/pam/main.hh"

int main(int argc, char** argv) {
	// The messages of the log are written to stderr too, which goes into a pipe to see whether there was one
	openlog("timeout_test", LOG_PERROR, LOG_USER);
	const bool delay = argc > 1 && std::strcmp(argv[1], "--delay") == 0;
	for (int i = delay ? 2 : 1; i < argc; i++) {
		int ends[2];
		if (pipe(ends) != 0) {
			return 1;
		}
		const int saved = dup(STDERR_FILENO);
		dup2(ends[1], STDERR_FILENO);
		const auto seconds = delay ? str_to_delay(argv[i], "delay") : str_to_seconds(argv[i]);
		dup2(saved, STDERR_FILENO);
		close(saved);
		close(ends[1]);
		char message[512];
		const bool warned = read(ends[0], message, sizeof(message)) > 0;
		close(ends[0]);
		std::printf("%s=%lld%s ", argv[i], static_cast<long long>(seconds.count()), warned ? "!" : "");
	}
	std::printf("\n");
	return 0;
}

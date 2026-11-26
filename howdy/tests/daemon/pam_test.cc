// Run pam_authenticate() of a PAM configuration with the pam_howdy.so of the
// test and print what the module tells the user and what it returns.
// Usage: pam_test <confdir> <service> <user>
//   PAM_TEST_REPEAT=<n>        authenticate n times with the same handle, one RESULT line each
//   PAM_TEST_SWAP=<from>:<to>  copy <from> to <to> before the second time (another face model)
//   PAM_TEST_PARALLEL=<service>  while the authentication runs, authenticate with this service on a handle of
//                              its own in a second thread, its result is a PARALLEL line
//   PAM_TEST_TTY_PROMPT=1      a password prompt waits for a line at the terminal (/dev/tty) without echo, as
//                              sudo does it, and an ECHO line says at the end whether the terminal echoes
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <security/pam_appl.h>

namespace {

// When a message came, in milliseconds since the start, written to the file in PAM_TEST_TIMES (tab, text)
void log_time(const char* text) {
	const char* path = std::getenv("PAM_TEST_TIMES");
	if (path == nullptr) {
		return;
	}
	static timespec start{};
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (start.tv_sec == 0 && start.tv_nsec == 0) {
		start = now;
	}
	FILE* file = std::fopen(path, "a");
	if (file != nullptr) {
		std::fprintf(file, "%ld\t%s\n", ((now.tv_sec - start.tv_sec) * 1000) + ((now.tv_nsec - start.tv_nsec) / 1000000), text);
		std::fclose(file);
	}
}

// Turns the echo of the terminal off and waits for a line. The flags are put back afterwards, a thread that is
// ended while it waits does not get there.
void read_without_echo() {
	const int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	termios flags{};
	tcgetattr(fd, &flags);
	termios quiet = flags;
	quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
	tcsetattr(fd, TCSAFLUSH, &quiet);
	char line[64];
	if (read(fd, line, sizeof(line)) < 0) {
		std::printf("reading the terminal failed\n");
	}
	tcsetattr(fd, TCSAFLUSH, &flags);
	close(fd);
}

void print_echo() {
	const int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	termios flags{};
	if (fd >= 0 && tcgetattr(fd, &flags) == 0) {
		std::printf("ECHO %s\n", (flags.c_lflag & ECHO) != 0 ? "on" : "off");
	}
	if (fd >= 0) {
		close(fd);
	}
}

int conversation(int count, const struct pam_message** messages, struct pam_response** responses, void* /*data*/) {
	*responses = static_cast<struct pam_response*>(calloc(static_cast<size_t>(count), sizeof(struct pam_response)));
	for (int i = 0; i < count; ++i) {
		std::printf("MSG %d: %s\n", messages[i]->msg_style, messages[i]->msg);
		log_time(messages[i]->msg);
		// A wrong password for a prompt, nobody types here
		if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF || messages[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			if (std::getenv("PAM_TEST_TTY_PROMPT") != nullptr) {
				read_without_echo();
			}
			(*responses)[i].resp = strdup("wrong");
		}
	}
	return PAM_SUCCESS;
}

// Starts a handle for the service, authenticates `times` times with it and prints the results
bool authenticate(const char* confdir, const char* service, const char* user, int times, const char* label) {
	const struct pam_conv conv = {conversation, nullptr};
	pam_handle_t* handle = nullptr;
	if (pam_start_confdir(service, user, &conv, confdir, &handle) != PAM_SUCCESS) {
		std::printf("pam_start failed\n");
		return false;
	}
	int result = PAM_SUCCESS;
	for (int i = 0; i < times; i++) {
		const char* swap = std::getenv("PAM_TEST_SWAP");
		if (i == 1 && swap != nullptr) {
			const std::string files = swap;
			const auto colon = files.find(':');
			std::filesystem::copy_file(files.substr(0, colon), files.substr(colon + 1), std::filesystem::copy_options::overwrite_existing);
		}
		result = pam_authenticate(handle, 0);
		std::printf("%s %d %s\n", label, result, pam_strerror(handle, result));
		std::fflush(stdout);
	}
	pam_end(handle, result);
	return true;
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 4) {
		return 2;
	}
	log_time("start");
	std::thread parallel;
	const char* parallel_service = std::getenv("PAM_TEST_PARALLEL");
	if (parallel_service != nullptr) {
		parallel = std::thread([&] {
			// After the first one has started
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			authenticate(argv[1], parallel_service, argv[3], 1, "PARALLEL");
		});
	}
	const char* repeat = std::getenv("PAM_TEST_REPEAT");
	const bool started = authenticate(argv[1], argv[2], argv[3], repeat != nullptr ? std::atoi(repeat) : 1, "RESULT");
	if (parallel.joinable()) {
		parallel.join();
	}
	if (std::getenv("PAM_TEST_TTY_PROMPT") != nullptr) {
		print_echo();
	}
	return started ? 0 : 2;
}

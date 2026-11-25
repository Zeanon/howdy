// Cancels FaceAuthenticator::authenticate() (while the daemon does not answer StartAuthentication, or during
// the scan), and prints the result and the time it took in milliseconds.
// Usage: face_cancel_test <user> <milliseconds until cancel()>
#include "../../src/pam/face_authenticator.hh"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
	if (argc != 3) {
		return 2;
	}
	FaceAuthenticator authenticator(argv[1], {});
	const auto start = std::chrono::steady_clock::now();
	std::thread canceller([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
		authenticator.cancel();
	});
	const auto result = authenticator.authenticate([](int, const char*) { return 0; }, [] {}, 1);
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	canceller.join();
	std::printf("RESULT %s %ld\n", result == FaceAuthenticator::Result::Cancelled ? "cancelled" : "other", static_cast<long>(took));
	return 0;
}

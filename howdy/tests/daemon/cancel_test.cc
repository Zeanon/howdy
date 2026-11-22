// Test of the calls of the management client that can be given up: compare_face() waits for a
// daemon that waits for polkit, and has to end in a short time when it is cancelled.
// Usage: cancel_test <user> cancel|wait|error   (the system bus is DBUS_SYSTEM_BUS_ADDRESS)
//
// Prints one line with the result and the milliseconds it took.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>

#include "management_client.hh"
#include "polkit.hh"

namespace {

long long milliseconds_since(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 3) {
		return 2;
	}
	const std::string user = argv[1];
	const std::string what = argv[2];
	sd_bus* bus = nullptr;
	if (sd_bus_open_system(&bus) < 0) {
		std::puts("open failed");
		return 1;
	}
	const std::vector<double> face{0.1, 0.2, 0.3};
	const auto start = std::chrono::steady_clock::now();

	if (what == "cancel") {
		// The daemon asks a polkit that takes five seconds, the call is cancelled after 300 ms
		std::atomic<bool> cancelled{false};
		std::thread canceller([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			cancelled = true;
		});
		try {
			howdy::compare_face(bus, user, "euclidean", face, howdy::POLKIT_TIMEOUT_USEC, [&] { return cancelled.load(); });
			std::printf("answered %lld\n", milliseconds_since(start));
		} catch (const howdy::CallCancelled&) {
			std::printf("cancelled %lld\n", milliseconds_since(start));
		} catch (const std::exception& exc) {
			std::printf("error %s\n", exc.what());
		}
		canceller.join();
	} else {
		// Not cancelled: the call waits for the answer, an error of the daemon comes through too
		const std::string metric = what == "error" ? "cosine" : "euclidean";
		try {
			const auto match = howdy::compare_face(bus, user, metric, face, howdy::POLKIT_TIMEOUT_USEC, [] { return false; });
			std::printf("answered %s %lld\n", match.has_value() ? match->label.c_str() : "no-match", milliseconds_since(start));
		} catch (const howdy::CallCancelled&) {
			std::puts("cancelled");
		} catch (const std::exception& exc) {
			std::printf("error %s\n", exc.what());
		}
	}

	sd_bus_flush_close_unref(bus);
	return 0;
}

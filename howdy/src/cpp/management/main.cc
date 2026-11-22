// Howdy management D-Bus daemon
//
// Exports the management interface (paths::MANAGEMENT_INTERFACE) to add, list, remove and clear the
// face models of a user. The model files are only accessible for root, polkit
// decides who may manage the models of a user.

#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "model_store.hh"
#include "paths.hh"
#include "service.hh"
#include "util.hh"

namespace {

struct EventDeleter {
	void operator()(sd_event* event) const {
		sd_event_unref(event);
	}
};
struct BusDeleter {
	void operator()(sd_bus* bus) const {
		sd_bus_flush_close_unref(bus);
	}
};

int fail(const char* what, int r) {
	howdy::log(LOG_ERR, std::string(what) + ": " + std::strerror(-r));
	return 1;
}

// --encrypt-models: encrypt the models that are still plain JSON, without the bus
int encrypt_models() {
	try {
		const auto report = howdy::ModelStore(howdy::paths::USER_MODELS_DIR).encrypt_all();
		std::cout << report.encrypted << " model file(s) encrypted, " << report.already_encrypted << " already encrypted\n";
		for (const auto& failure : report.failures) {
			std::cerr << "Not encrypted: " << failure << "\n";
		}
		return report.failures.empty() ? 0 : 1;
	} catch (const std::exception& exc) {
		std::cerr << "Failed to encrypt the models: " << exc.what() << "\n";
		return 1;
	}
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 2 && std::string_view(argv[1]) == "--encrypt-models") {
		return encrypt_models();
	}
	if (argc != 1) {
		std::cerr << "Usage: " << argv[0] << " [--encrypt-models]\n";
		return 2;
	}
	openlog("howdy-management", LOG_PID, LOG_AUTH);

	// The termination signals are handled by the event loop
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, nullptr);

	sd_event* raw_event = nullptr;
	int r = sd_event_default(&raw_event);
	if (r < 0) {
		return fail("Failed to create the event loop", r);
	}
	const std::unique_ptr<sd_event, EventDeleter> event(raw_event);

	// Without a handler sd-event exits the loop on these signals
	sd_event_add_signal(event.get(), nullptr, SIGTERM, nullptr, nullptr);
	sd_event_add_signal(event.get(), nullptr, SIGINT, nullptr, nullptr);

	sd_bus* raw_bus = nullptr;
	r = sd_bus_open_system(&raw_bus);
	if (r < 0) {
		return fail("Failed to connect to the system bus", r);
	}
	const std::unique_ptr<sd_bus, BusDeleter> bus(raw_bus);

	r = sd_bus_attach_event(bus.get(), event.get(), SD_EVENT_PRIORITY_NORMAL);
	if (r < 0) {
		return fail("Failed to attach the bus to the event loop", r);
	}

	try {
		howdy::ManagementService service(bus.get(), event.get(), howdy::ModelStore(howdy::paths::USER_MODELS_DIR));

		r = sd_bus_request_name(bus.get(), howdy::paths::MANAGEMENT_BUS, 0);
		if (r < 0) {
			return fail("Failed to acquire the bus name", r);
		}

		howdy::log(LOG_INFO, "Howdy Management D-Bus service started");

		r = sd_event_loop(event.get());
		if (r < 0) {
			return fail("Event loop failed", r);
		}
	} catch (const std::exception& exc) {
		howdy::log(LOG_ERR, std::string("Howdy management daemon failed: ") + exc.what());
		return 1;
	}

	return 0;
}

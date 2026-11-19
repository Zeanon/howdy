// Howdy authentication D-Bus daemon
//
// Exports org.boltgolt.howdy.Authentication for the PAM module and emits the
// signals for the authentication popup (org.boltgolt.howdy.AuthPopup).

#include <csignal>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "authenticator.hh"
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

} // namespace

int main() {
	openlog("howdy-authentication", LOG_PID, LOG_AUTH);

	// Block the termination signals before any thread is started, so they are
	// only delivered through the event loop.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	pthread_sigmask(SIG_BLOCK, &mask, nullptr);

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
		// Init the services once.
		howdy::PopupSignals popup(bus.get(), event.get());
		howdy::Authenticator authenticator(popup);
		howdy::AuthenticationService service(bus.get(), event.get(), authenticator);

		r = sd_bus_request_name(bus.get(), howdy::paths::BUS_NAME, 0);
		if (r < 0) {
			return fail("Failed to acquire the bus name", r);
		}

		howdy::log(LOG_INFO, "Howdy Authentication D-Bus service is ready");

		r = sd_event_loop(event.get());
		service.shutdown();
		if (r < 0) {
			return fail("Event loop failed", r);
		}
	} catch (const std::exception& exc) {
		howdy::log(LOG_ERR, std::string("Howdy authentication daemon failed: ") + exc.what());
		return 1;
	}

	howdy::log(LOG_INFO, "Howdy Authentication D-Bus service stopped");
	return 0;
}

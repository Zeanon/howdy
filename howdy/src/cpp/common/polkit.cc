#include "polkit.hh"

#include <cstring>
#include <memory>

#include "caller.hh"
#include "paths.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr const char* POLKIT_BUS = "org.freedesktop.PolicyKit1";
constexpr const char* POLKIT_PATH = "/org/freedesktop/PolicyKit1/Authority";
constexpr const char* POLKIT_INTERFACE = "org.freedesktop.PolicyKit1.Authority";

// The polkit action has the name of the management interface
const char* const POLKIT_ACTION = paths::MANAGEMENT_INTERFACE;
constexpr std::uint32_t POLKIT_ALLOW_USER_INTERACTION = 1;

struct MessageDeleter {
	void operator()(sd_bus_message* message) const {
		sd_bus_message_unref(message);
	}
};
using MessagePtr = std::unique_ptr<sd_bus_message, MessageDeleter>;

void check(int r, const char* what) {
	if (r < 0) {
		throw std::runtime_error(std::string(what) + ": " + std::strerror(-r));
	}
}

} // namespace

void check_caller_may_manage(uid_t caller_uid, const std::string& target_user) {
	const auto target_uid = lookup_uid(target_user);
	if (!target_uid.has_value()) {
		throw std::runtime_error("Unknown user: " + target_user);
	}
	if (!may_access(caller_uid, *target_uid)) {
		throw AuthorizationError("Not authorized to modify models for user " + target_user);
	}
}

sd_bus_message* new_check_authorization_call(sd_bus* bus, const std::string& bus_name) {
	sd_bus_message* raw = nullptr;
	check(sd_bus_message_new_method_call(bus, &raw, POLKIT_BUS, POLKIT_PATH, POLKIT_INTERFACE, "CheckAuthorization"), "Could not create the polkit call");
	MessagePtr message(raw);

	// subject: (sa{sv}), the bus name of the client
	check(sd_bus_message_open_container(raw, SD_BUS_TYPE_STRUCT, "sa{sv}"), "Could not build the polkit call");
	check(sd_bus_message_append(raw, "s", "system-bus-name"), "Could not build the polkit call");
	check(sd_bus_message_open_container(raw, SD_BUS_TYPE_ARRAY, "{sv}"), "Could not build the polkit call");
	check(sd_bus_message_append(raw, "{sv}", "name", "s", bus_name.c_str()), "Could not build the polkit call");
	check(sd_bus_message_close_container(raw), "Could not build the polkit call");
	check(sd_bus_message_close_container(raw), "Could not build the polkit call");

	// action id, no details, flags, no cancellation id
	check(sd_bus_message_append(raw, "s", POLKIT_ACTION), "Could not build the polkit call");
	check(sd_bus_message_append(raw, "a{ss}", 0), "Could not build the polkit call");
	check(sd_bus_message_append(raw, "us", POLKIT_ALLOW_USER_INTERACTION, ""), "Could not build the polkit call");

	return message.release();
}

void evaluate_check_authorization_reply(sd_bus_message* reply) {
	if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
		const sd_bus_error* error = sd_bus_message_get_error(reply);
		throw std::runtime_error(
			std::string("Polkit authorization failed: ") + (error != nullptr && error->message != nullptr ? error->message : "unknown error"));
	}

	if (sd_bus_message_has_signature(reply, "(bba{ss})") == 0) {
		throw std::runtime_error("Unexpected Polkit reply signature");
	}

	check(sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "bba{ss}"), "Could not read the polkit reply");
	int authorized = 0;
	int challenge = 0;
	check(sd_bus_message_read(reply, "bb", &authorized, &challenge), "Could not read the polkit reply");

	if (authorized == 0) {
		throw AuthorizationError(std::string("Polkit authorization denied") + (challenge != 0 ? " (authentication required)" : ""));
	}
}

void check_authorization(sd_bus* bus, uid_t caller_uid, const std::string& target_user) {
	check_caller_may_manage(caller_uid, target_user);

	const char* unique_name = nullptr;
	check(sd_bus_get_unique_name(bus, &unique_name), "Could not get the bus name");

	const MessagePtr call(new_check_authorization_call(bus, unique_name));

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* raw_reply = nullptr;
	const int r = sd_bus_call(bus, call.get(), POLKIT_TIMEOUT_USEC, &error, &raw_reply);
	const MessagePtr reply(raw_reply);
	if (r < 0) {
		const std::string text = error.message != nullptr ? error.message : std::strerror(-r);
		sd_bus_error_free(&error);
		throw std::runtime_error("Polkit authorization failed: " + text);
	}
	sd_bus_error_free(&error);

	evaluate_check_authorization_reply(reply.get());
}

} // namespace howdy

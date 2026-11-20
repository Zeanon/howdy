#include "service.hh"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "caller.hh"
#include "paths.hh"
#include "polkit.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr std::uint64_t USEC_PER_SEC = 1000000ULL;

struct Method {
	std::string_view name;
	// Signature without the optional leading user name
	std::string_view arguments;
	const char* error;
	const char* failure;
};

// What a failed authorization is answered with, so that clients can tell it
// from other failures
constexpr const char* NOT_AUTHORIZED_ERROR = paths::MANAGEMENT_NOT_AUTHORIZED_ERROR;

constexpr std::array<Method, 4> METHODS{{
	{"Add", "s", paths::MANAGEMENT_ADD_ERROR, "Failed to add face model"},
	{"List", "", paths::MANAGEMENT_LIST_ERROR, "Failed to list face models"},
	{"Remove", "s", paths::MANAGEMENT_REMOVE_ERROR, "Failed to remove face model"},
	{"Clear", "", paths::MANAGEMENT_CLEAR_ERROR, "Failed to clear face models"},
}};

void check(int r, const char* what) {
	if (r < 0) {
		throw std::runtime_error(std::string(what) + ": " + std::strerror(-r));
	}
}

const Method* find_method(const char* interface, const char* member) {
	if (interface == nullptr || member == nullptr || std::strcmp(interface, paths::MANAGEMENT_INTERFACE) != 0) {
		return nullptr;
	}
	for (const auto& method : METHODS) {
		if (method.name == member) {
			return &method;
		}
	}
	return nullptr;
}

void reply_error(sd_bus_message* message, const char* name, const std::string& text) {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_error_set(&error, name, text.c_str());
	sd_bus_reply_method_error(message, &error);
	sd_bus_error_free(&error);
}

} // namespace

// A call that waits for the polkit authorization
struct ManagementService::Request {
	ManagementService* service;
	sd_bus_message* message;
	const Method* method;
	std::string username;
	// Model (Add) or id (Remove), unused for the others
	std::string argument;
};

ManagementService::ManagementService(sd_bus* bus, sd_event* event, ModelStore store) : bus_(bus), event_(event), store_(std::move(store)) {
	// All methods exist with and without a user name as first argument, which
	// a vtable can not express. Handle the calls of the path ourselves.
	check(sd_bus_add_object(bus_, &slot_, paths::MANAGEMENT_PATH, on_message, this), "Failed to export the management interface");

	check(
		sd_event_add_time_relative(event_, &idle_timer_, CLOCK_MONOTONIC, static_cast<std::uint64_t>(IDLE_TIMEOUT.count()) * USEC_PER_SEC, 0, on_idle, this),
		"Failed to add the idle timer");
	check(sd_event_source_set_enabled(idle_timer_, SD_EVENT_ONESHOT), "Failed to enable the idle timer");
}

ManagementService::~ManagementService() {
	sd_event_source_disable_unref(idle_timer_);
	sd_bus_slot_unref(slot_);
}

int ManagementService::on_message(sd_bus_message* message, void* userdata, sd_bus_error* /*ret_error*/) {
	return static_cast<ManagementService*>(userdata)->handle(message);
}

int ManagementService::handle(sd_bus_message* message) {
	uint8_t type = 0;
	if (sd_bus_message_get_type(message, &type) < 0 || type != SD_BUS_MESSAGE_METHOD_CALL || stopping_) {
		return 0;
	}

	const Method* method = find_method(sd_bus_message_get_interface(message), sd_bus_message_get_member(message));
	if (method == nullptr) {
		return 0;
	}

	const std::string signature = sd_bus_message_get_signature(message, 1);
	const bool with_user = signature == "s" + std::string(method->arguments);
	if (signature != method->arguments && !with_user) {
		reply_error(message, SD_BUS_ERROR_INVALID_ARGS, "Invalid arguments for " + std::string(method->name));
		return 1;
	}

	// Stop the idle timer while a request is processed
	++pending_;
	sd_event_source_set_enabled(idle_timer_, SD_EVENT_OFF);

	auto request = std::make_unique<Request>(Request{this, message, method, "", ""});
	try {
		if (with_user) {
			const char* user = nullptr;
			check(sd_bus_message_read(message, "s", &user), "Failed to read the user");
			request->username = user;
		}
		if (!method->arguments.empty()) {
			const char* argument = nullptr;
			check(sd_bus_message_read(message, "s", &argument), "Failed to read the argument");
			request->argument = argument;
		}

		// The caller is identified by the bus, not by what it claims
		uid_t caller_uid = 0;
		check(query_caller_uid(message, caller_uid), "Failed to identify the caller");

		if (!with_user) {
			const auto name = lookup_username(caller_uid);
			if (!name.has_value()) {
				throw std::runtime_error("Could not determine the user of the caller");
			}
			request->username = *name;
		}

		check_caller_may_manage(caller_uid, request->username);

		const char* sender = sd_bus_message_get_sender(message);
		if (sender == nullptr) {
			throw AuthorizationError("Missing D-Bus sender");
		}

		sd_bus_message* call = new_check_authorization_call(bus_, sender);
		const int r = sd_bus_call_async(bus_, nullptr, call, on_authorization, request.get(), POLKIT_TIMEOUT_USEC);
		sd_bus_message_unref(call);
		check(r, "Failed to call polkit");

		// The request now belongs to the pending call, on_authorization takes it back
		static_cast<void>(sd_bus_message_ref(message));
		request.release(); // NOLINT(bugprone-unused-return-value)
	} catch (const AuthorizationError& exc) {
		log(LOG_ERR, std::string(method->failure) + ": " + exc.what());
		reply_error(message, NOT_AUTHORIZED_ERROR, exc.what());
		--pending_;
		arm_idle_timer();
	} catch (const std::exception& exc) {
		log(LOG_ERR, std::string(method->failure) + ": " + exc.what());
		reply_error(message, method->error, exc.what());
		--pending_;
		arm_idle_timer();
	}
	return 1;
}

int ManagementService::on_authorization(sd_bus_message* reply, void* userdata, sd_bus_error* /*ret_error*/) {
	// Takes the request over from the pending call
	const std::unique_ptr<Request> request(static_cast<Request*>(userdata));
	ManagementService* self = request->service;

	try {
		evaluate_check_authorization_reply(reply);
		self->execute(*request);
	} catch (const AuthorizationError& exc) {
		log(LOG_ERR, std::string(request->method->failure) + ": " + exc.what());
		reply_error(request->message, NOT_AUTHORIZED_ERROR, exc.what());
	} catch (const std::exception& exc) {
		log(LOG_ERR, std::string(request->method->failure) + ": " + exc.what());
		reply_error(request->message, request->method->error, exc.what());
	}

	sd_bus_message_unref(request->message);
	--self->pending_;
	self->arm_idle_timer();
	return 0;
}

void ManagementService::execute(const Request& request) {
	const std::string_view name = request.method->name;

	if (name == "Add") {
		const auto model = nlohmann::json::parse(request.argument, nullptr, false);
		if (model.is_discarded()) {
			throw std::runtime_error("Model is not valid JSON");
		}
		store_.add(request.username, model);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	} else if (name == "List") {
		check(sd_bus_reply_method_return(request.message, "s", store_.list(request.username).c_str()), "Failed to send the reply");
	} else if (name == "Remove") {
		store_.remove(request.username, request.argument);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	} else {
		store_.clear(request.username);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	}
}

void ManagementService::arm_idle_timer() {
	if (pending_ > 0 || stopping_) {
		return;
	}
	sd_event_source_set_time_relative(idle_timer_, static_cast<std::uint64_t>(IDLE_TIMEOUT.count()) * USEC_PER_SEC);
	sd_event_source_set_enabled(idle_timer_, SD_EVENT_ONESHOT);
}

int ManagementService::on_idle(sd_event_source* /*source*/, std::uint64_t /*usec*/, void* userdata) {
	auto* self = static_cast<ManagementService*>(userdata);
	if (self->pending_ == 0) {
		self->stopping_ = true;
		log(LOG_INFO, "Howdy Management D-Bus service stopped after inactivity");
		sd_event_exit(self->event_, 0);
	}
	return 0;
}

} // namespace howdy

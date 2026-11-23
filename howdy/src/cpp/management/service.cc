#include "service.hh"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

constexpr std::array<Method, 7> METHODS{{
	{"Add", "s", paths::MANAGEMENT_ADD_ERROR, "Failed to add face model"},
	{"List", "", paths::MANAGEMENT_LIST_ERROR, "Failed to list face models"},
	{"Remove", "sss", paths::MANAGEMENT_REMOVE_ERROR, "Failed to remove face model"},
	{"Rename", "ssss", paths::MANAGEMENT_RENAME_ERROR, "Failed to rename face model"},
	{"Clear", "", paths::MANAGEMENT_CLEAR_ERROR, "Failed to clear face models"},
	{"Compare", "sad", paths::MANAGEMENT_COMPARE_ERROR, "Failed to compare the face"},
	{"KeepAlive", "", paths::MANAGEMENT_KEEPALIVE_ERROR, "Failed to keep the authorization alive"},
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

// The array of doubles of a call
void read_encoding(sd_bus_message* message, std::vector<double>& encoding) {
	const void* data = nullptr;
	std::size_t size = 0;
	check(sd_bus_message_read_array(message, 'd', &data, &size), "Failed to read the face");
	if (size % sizeof(double) != 0) {
		throw std::runtime_error("Invalid face encoding");
	}
	encoding.resize(size / sizeof(double));
	if (size != 0) {
		std::memcpy(encoding.data(), data, size);
	}
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
	// Model (Add) or id (Remove, Rename), unused for the others
	std::string argument;
	// Remove, Rename: the label and time the caller saw the model with, the model is only changed
	// if it still has them (the ids move up when a model is removed)
	std::string expected_label;
	std::string expected_time;
	// Compare: the face
	std::vector<double> encoding;
	// The bus name of the caller, for Compare
	std::string sender;
	// Rename: the new label
	std::string label;
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

	auto request = std::make_unique<Request>(Request{this, message, method, "", "", "", "", {}, "", ""});
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
		if (method->name == "Remove" || method->name == "Rename") {
			const char* expected = nullptr;
			check(sd_bus_message_read(message, "s", &expected), "Failed to read the label of the model");
			request->expected_label = expected;
			const char* time = nullptr;
			check(sd_bus_message_read(message, "s", &time), "Failed to read the time of the model");
			request->expected_time = time;
		}
		if (method->name == "Rename") {
			const char* label = nullptr;
			check(sd_bus_message_read(message, "s", &label), "Failed to read the label");
			request->label = label;
		}
		if (method->name == "Compare") {
			read_encoding(message, request->encoding);
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

		// A user that calls Compare too often is told to slow down, before polkit is asked (a program
		// that does not stop would otherwise have a password prompt for every call) or a model is read
		if (method->name == "Compare" && !compare_limiter_.allow(caller_uid, std::chrono::steady_clock::now())) {
			const auto now = std::chrono::steady_clock::now();
			if (now - last_rate_limit_log_ > std::chrono::minutes(1)) {
				last_rate_limit_log_ = now;
				log(LOG_WARNING, "Compare is called too often by the user " + std::to_string(caller_uid) + ", slowing it down");
			}
			reply_error(message, paths::MANAGEMENT_COMPARE_RATE_LIMITED_ERROR, "Too many comparisons, please slow down");
			--pending_;
			arm_idle_timer();
			return 1;
		}

		const char* sender = sd_bus_message_get_sender(message);
		if (sender == nullptr) {
			throw AuthorizationError("Missing D-Bus sender");
		}

		// KeepAlive only extends what polkit allowed to this connection, it allows nothing itself
		if (method->name == "KeepAlive") {
			request->sender = sender;
			execute(*request);
			--pending_;
			arm_idle_timer();
			return 1;
		}

		// Compare is called for every frame of howdy test: polkit is asked on the first call
		// of a connection only, the caller was checked to be root or the user above
		if (method->name == "Compare") {
			request->sender = sender;
			if (compare_authorized(request->sender)) {
				execute(*request);
				--pending_;
				arm_idle_timer();
				return 1;
			}
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
		if (request->method->name == "Compare") {
			self->remember_compare(request->sender);
		}
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
		store_.remove(request.username, request.argument, request.expected_label, request.expected_time);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	} else if (name == "Rename") {
		store_.rename(request.username, request.argument, request.expected_label, request.expected_time, request.label);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	} else if (name == "KeepAlive") {
		check(sd_bus_reply_method_return(request.message, "b", compare_authorized(request.sender) ? 1 : 0), "Failed to send the reply");
	} else if (name == "Compare") {
		Metric metric{};
		if (request.argument == "euclidean") {
			metric = Metric::Euclidean;
		} else if (request.argument == "dot") {
			metric = Metric::Dot;
		} else {
			throw std::runtime_error("Unknown metric " + request.argument);
		}
		const auto match = store_.compare(request.username, metric, request.encoding);
		check(
			sd_bus_reply_method_return(
				request.message, "bds", match.has_value() ? 1 : 0, match.has_value() ? match->value : 0.0, match.has_value() ? match->label.c_str() : ""),
			"Failed to send the reply");
	} else {
		store_.clear(request.username);
		check(sd_bus_reply_method_return(request.message, "b", 1), "Failed to send the reply");
	}
}

bool ManagementService::compare_authorized(const std::string& sender) {
	const auto now = std::chrono::steady_clock::now();
	std::erase_if(compare_authorized_, [&](const auto& entry) { return entry.second < now; });

	const auto found = compare_authorized_.find(sender);
	if (found == compare_authorized_.end()) {
		return false;
	}
	// Used again, so it stays
	found->second = now + COMPARE_KEEPALIVE_EXTENSION;
	return true;
}

void ManagementService::remember_compare(const std::string& sender) {
	compare_authorized_[sender] = std::chrono::steady_clock::now() + COMPARE_INITIAL_KEEPALIVE;
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

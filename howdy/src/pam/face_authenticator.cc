#include "face_authenticator.hh"

#include "main.hh"
#include <vars.hh>

#include <atomic>
#include <chrono>
#include <libintl.h>
#include <security/pam_ext.h>
#include <syslog.h>

#include <thread>
#include <utility>

#define S(msg) gettext(msg)

namespace {
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
constexpr auto NO_MATCH_WAIT_TIME = std::chrono::milliseconds(500);
// The daemon is restarted when the computer wakes up and needs a few seconds to load. An authentication
// that meets this waits for it so long (all in all, not for each time it goes away), then it fails.
constexpr auto RECONNECT_TIMEOUT = std::chrono::seconds(20);
constexpr auto RECONNECT_INTERVAL = std::chrono::milliseconds(250);

// The call could not be answered because nobody (or nobody in time) was there to answer it
bool daemon_unreachable(int r, const sd_bus_error& error) {
	return sd_bus_error_has_name(&error, SD_BUS_ERROR_SERVICE_UNKNOWN) != 0 || sd_bus_error_has_name(&error, SD_BUS_ERROR_NAME_HAS_NO_OWNER) != 0 ||
		   sd_bus_error_has_name(&error, SD_BUS_ERROR_NO_REPLY) != 0 || sd_bus_error_has_name(&error, SD_BUS_ERROR_DISCONNECTED) != 0 ||
		   sd_bus_error_has_name(&error, SD_BUS_ERROR_TIMEOUT) != 0 || r == -ETIMEDOUT || r == -ENOTCONN || r == -ECONNRESET;
}
} // namespace

FaceAuthenticator::FaceAuthenticator(std::string username, std::vector<char const*> args) : username_(std::move(username)), args_(std::move(args)) {
}

FaceAuthenticator::~FaceAuthenticator() {
	cancel_request();
	cleanup();
}

bool FaceAuthenticator::connect() {
	const int r = sd_bus_open_system(&bus_);

	if (r < 0) {
		syslog(LOG_ERR, "Face: D-Bus connection failed: %s", strerror(-r));
		result_ = Result::Disconnected;
		return false;
	}

	const int timeout_result = sd_bus_set_method_call_timeout(bus_, 2ULL * 1000000ULL);

	if (timeout_result < 0) {
		result_ = Result::Error;
		return false;
	}

	return sd_bus_match_signal(
			   bus_, &name_owner_slot_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", howdy_name_owner_changed,
			   this) >= 0;
}

bool FaceAuthenticator::start_authentication() {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* message = nullptr;
	sd_bus_message* reply = nullptr;

	int r = sd_bus_message_new_method_call(bus_, &message, HOWDY_BUS, AUTHENTICATION_PATH, AUTHENTICATION_INTERFACE, "StartAuthentication");

	if (r >= 0) {
		r = sd_bus_message_append(message, "s", username_.c_str());
	}

	if (r >= 0) {
		r = sd_bus_message_open_container(message, 'a', "s");
	}

	for (const auto& arg : args_) {
		r = sd_bus_message_append(message, "s", arg);
	}

	if (r >= 0) {
		r = sd_bus_message_close_container(message);
	}

	if (r >= 0) {
		r = sd_bus_call(bus_, message, 0, &error, &reply);
	}

	if (r < 0) {
		if (sd_bus_error_has_name(&error, "org.howdy.Authentication.Busy")) {
			result_ = Result::Busy;
		} else if (sd_bus_error_has_name(&error, "org.howdy.Authentication.InvalidUser")) {
			result_ = Result::InvalidUser;
		} else if (daemon_unreachable(r, error)) {
			result_ = Result::Disconnected;
			daemon_unreachable_ = true;
		} else {
			result_ = Result::Error;
			syslog(LOG_ERR, "Face: StartAuthentication failed: %s", error.message ? error.message : strerror(-r));
		}

		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return false;
	}

	const char* request_id = nullptr;
	const int read_result = sd_bus_message_read(reply, "s", &request_id);

	if (read_result < 0 || request_id == nullptr) {
		result_ = Result::Error;
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return false;
	}

	request_id_ = request_id;
	request_started_ = true;

	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);

	return true;
}

bool FaceAuthenticator::start_authentication_waiting(std::chrono::steady_clock::time_point deadline) {
	bool announced = false;
	for (;;) {
		daemon_unreachable_ = false;
		if (start_authentication()) {
			// A daemon that was reported gone before this request is the one that answered it
			daemon_lost_ = false;
			return true;
		}
		if (!daemon_unreachable_ || cancelled_ || std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		if (!announced) {
			announced = true;
			syslog(LOG_NOTICE, "Face: the daemon is not there, waiting for it");
		}
		// In small steps, a cancel (a password was typed) is not held up
		for (auto waited = std::chrono::milliseconds(0); waited < RECONNECT_INTERVAL && !cancelled_; waited += POLL_INTERVAL) {
			std::this_thread::sleep_for(POLL_INTERVAL);
		}
	}
}

inline FaceAuthenticator::Result FaceAuthenticator::map_status(const char* status) noexcept {
	if (str_equal(status, "SUCCESS")) {
		return Result::Success;
	}

	if (str_equal(status, "NO_MATCH")) {
		return Result::NoMatch;
	}

	if (str_equal(status, "NO_ENROLLED_FACE")) {
		return Result::NoEnrolledFace;
	}

	if (str_equal(status, "TIMEOUT")) {
		return Result::Timeout;
	}

	if (str_equal(status, "TOO_DARK")) {
		return Result::TooDark;
	}

	if (str_equal(status, "CANCELLED") || str_equal(status, "CANCELLING")) {
		return Result::Cancelled;
	}

	if (str_equal(status, "ERROR")) {
		return Result::Error;
	}

	if (str_equal(status, "NOT_INITIALIZED")) {
		return Result::NotInitialized;
	}

	if (str_equal(status, "NO_DEVICE")) {
		return Result::NoDevice;
	}

	return Result::Error;
}

void FaceAuthenticator::cancel_request() {
	if (!bus_ || !request_started_ || request_id_.empty()) {
		return;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	const int r =
		sd_bus_call_method(bus_, HOWDY_BUS, AUTHENTICATION_PATH, AUTHENTICATION_INTERFACE, "CancelAuthentication", &error, &reply, "s", request_id_.c_str());

	if (r < 0) {
		syslog(LOG_WARNING, "Face: CancelAuthentication failed: %s", error.message ? error.message : strerror(-r));
	}

	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
}

void FaceAuthenticator::cleanup() {
	if (bus_) {
		// Nothing to clear at a daemon that is gone (and the call would have the bus start it)
		if (request_started_ && !daemon_lost_) {
			sd_bus_error error = SD_BUS_ERROR_NULL;
			sd_bus_message* reply = nullptr;
			const int r = sd_bus_call_method(
				bus_, HOWDY_BUS, AUTHENTICATION_PATH, AUTHENTICATION_INTERFACE, "ClearAuthentication", &error, &reply, "s", request_id_.c_str());

			if (r < 0) {
				syslog(LOG_WARNING, "Face: ClearAuthentication failed: %s", error.message ? error.message : strerror(-r));
			}

			sd_bus_message_unref(reply);
			sd_bus_error_free(&error);
		}

		sd_bus_flush_close_unref(bus_);
		bus_ = nullptr;
	}

	request_started_ = false;
	request_id_.clear();
}

void FaceAuthenticator::process_events(const std::function<void()>& on_scanning) {
	int r;
	while (!cancelled_ && !daemon_lost_) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message* reply = nullptr;

		r = sd_bus_call_method(bus_, HOWDY_BUS, AUTHENTICATION_PATH, AUTHENTICATION_INTERFACE, "GetResult", &error, &reply, "s", request_id_.c_str());

		if (r < 0) {
			// Both mean that the daemon went away, and came back without the request or is not back yet
			daemon_lost_ = true;
			if (sd_bus_error_has_name(&error, "org.howdy.Authentication.UnknownRequest")) {
				result_ = Result::UnknownRequest;
			} else {
				result_ = Result::Disconnected;
				syslog(LOG_ERR, "Face: GetResult failed: %s", error.message ? error.message : strerror(-r));
			}

			sd_bus_message_unref(reply);
			sd_bus_error_free(&error);
			return;
		}

		const char* status = nullptr;
		const int read_result = sd_bus_message_read(reply, "s", &status);

		if (read_result < 0 || status == nullptr) {
			result_ = Result::Error;
			sd_bus_message_unref(reply);
			sd_bus_error_free(&error);
			return;
		}

		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);

		if (!str_equal(status, "RUNNING") && !str_equal(status, "CANCELLING")) {
			result_ = map_status(status);
			return;
		}

		// A user without a face model is answered at once, only now it is known that the camera is used
		if (!scanning_notified_ && str_equal(status, "RUNNING")) {
			scanning_notified_ = true;
			if (on_scanning) {
				on_scanning();
			}
		}

		std::this_thread::sleep_for(POLL_INTERVAL);
	}

	if (cancelled_) {
		cancel_request();
		result_ = Result::Cancelled;
	} else if (daemon_lost_) {
		// There is no request left to cancel, authenticate() decides if it waits for the daemon
		result_ = Result::Disconnected;
	}
}

FaceAuthenticator::Result
FaceAuthenticator::authenticate(const std::function<int(int, const char*)>& conv_function, const std::function<void()>& on_scanning, int max_tries) {
	result_ = Result::Error;
	scanning_notified_ = false;

	if (!connect()) {
		return result_;
	}

	// All the waiting for a daemon that is not there is counted together
	const auto reconnect_deadline = std::chrono::steady_clock::now() + RECONNECT_TIMEOUT;
	int retries = 0;
	bool no_match = true;
	bool terminated = cancelled_;
	while (retries < max_tries && no_match && !terminated) {
		daemon_lost_ = false;
		if (!start_authentication_waiting(reconnect_deadline)) {
			cleanup();
			return result_;
		}

		process_events(on_scanning);

		// The daemon went away during the request (it is restarted, for example after the computer woke up):
		// a new request, as long as it is back in time, and this is not one of the tries
		if ((result_ == Result::Disconnected || result_ == Result::UnknownRequest) && !cancelled_ && std::chrono::steady_clock::now() < reconnect_deadline) {
			syslog(LOG_NOTICE, "Face: the daemon went away, starting the request again");
			request_started_ = false;
			request_id_.clear();
			continue;
		}

		if (result_ == Result::NoDevice) {
			conv_function(PAM_ERROR_MSG, S("Howdy could not find a camera device at the path specified in the config file."));
		}

		retries++;
		no_match = (result_ == Result::NoMatch || result_ == Result::Timeout);
		if (no_match && !cancelled_) {
			syslog(
				LOG_ERR,
				"Face authentication failed "
				"(attempt %d/%d)",
				retries, max_tries);
			if (retries < max_tries) {
				conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached, please try again"));
			} else {
				conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached"));
				result_ = Result::MaxTries;
			}
			std::this_thread::sleep_for(NO_MATCH_WAIT_TIME);
		}

		terminated = (cancelled_ || result_ == Result::Cancelled || result_ == Result::Disconnected);
	}
	cleanup();
	return result_;
}

int FaceAuthenticator::howdy_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*unused*/) {
	auto* self = static_cast<FaceAuthenticator*>(userdata);

	const char* name = nullptr;
	const char* old_owner = nullptr;
	const char* new_owner = nullptr;

	if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) < 0) {

		return 0;
	}

	if (!str_equal(name, HOWDY_BUS)) {
		return 0;
	}

	/*
	 * howdy disappeared.
	 */
	if (new_owner == nullptr || new_owner[0] == '\0') {
		self->daemon_lost_ = true;
	}

	return 0;
}

void FaceAuthenticator::cancel() noexcept {
	cancelled_ = true;
}

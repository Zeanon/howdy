#include "face_authenticator.hh"

#include "log.hh"
#include "main.hh"
#include "translate.hh"
#include <vars.hh>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <security/pam_ext.h>
#include <string>
#include <syslog.h>

#include <thread>
#include <utility>

#define S(msg) translate(msg)

namespace {
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
constexpr auto NO_MATCH_WAIT_TIME = std::chrono::milliseconds(500);
// The daemon is restarted when the computer wakes up and needs a few seconds to load. An authentication
// that meets this waits for it so long (all in all, not for each time it goes away), then it fails.
constexpr auto RECONNECT_TIMEOUT = std::chrono::seconds(20);
constexpr auto RECONNECT_INTERVAL = std::chrono::milliseconds(250);
// The daemon says Busy while another authentication of the user runs (or one that nobody asks for any more,
// until its keepalive ends). No request was made then, so it can be asked again for this long.
constexpr auto BUSY_TIMEOUT = std::chrono::seconds(5);

// The call could not be answered because the daemon went away or was not there. A timeout is not
// this: the daemon may have the request and just not have answered yet, a new request would be a second one.
bool daemon_unreachable(const std::string& error_name) {
	return error_name == SD_BUS_ERROR_SERVICE_UNKNOWN || error_name == SD_BUS_ERROR_NAME_HAS_NO_OWNER || error_name == SD_BUS_ERROR_NO_REPLY ||
		   error_name == SD_BUS_ERROR_DISCONNECTED;
}

// What the asynchronous call of StartAuthentication is answered with
struct StartReply {
	bool done = false;
	bool failed = false;
	std::string request_id;
	std::string error_name;
	std::string error_message;
};

int on_start_reply(sd_bus_message* reply, void* userdata, sd_bus_error* /*ret_error*/) {
	auto* state = static_cast<StartReply*>(userdata);
	state->done = true;

	if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
		const sd_bus_error* error = sd_bus_message_get_error(reply);
		state->failed = true;
		if (error != nullptr && error->name != nullptr) {
			state->error_name = error->name;
		}
		if (error != nullptr && error->message != nullptr) {
			state->error_message = error->message;
		}
		return 0;
	}

	const char* request_id = nullptr;
	if (sd_bus_message_read(reply, "s", &request_id) < 0 || request_id == nullptr) {
		state->failed = true;
		state->error_message = "The reply has no request id";
		return 0;
	}
	state->request_id = request_id;
	return 0;
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
		howdy_log(LOG_ERR, "Face: D-Bus connection failed: %s", strerror(-r));
		result_ = Result::Disconnected;
		return false;
	}

	const int timeout_result = sd_bus_set_method_call_timeout(bus_, 2ULL * 1000000ULL);

	if (timeout_result < 0) {
		result_ = Result::Error;
		return false;
	}

	return true;
}

bool FaceAuthenticator::start_authentication(std::chrono::steady_clock::time_point deadline) {
	sd_bus_message* message = nullptr;

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

	// The call is asynchronous and waits for its answer until the deadline, as long as it is not cancelled. A
	// daemon that has to be started by the bus (or is slow after the computer woke up) gets the call when it
	// is there, and it is the only request, a timeout and a second call would make two.
	StartReply state;
	sd_bus_slot* slot = nullptr;
	if (r >= 0) {
		const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
		const auto timeout = std::max<std::int64_t>(remaining.count(), 2000000);
		r = sd_bus_call_async(bus_, &slot, message, on_start_reply, &state, static_cast<std::uint64_t>(timeout));
	}
	sd_bus_message_unref(message);

	if (r < 0) {
		// Not connected any more: the same as a daemon that is gone
		if (r == -ENOTCONN || r == -ECONNRESET) {
			daemon_unreachable_ = true;
		}
		result_ = Result::Error;
		howdy_log(LOG_ERR, "Face: StartAuthentication failed: %s", strerror(-r));
		return false;
	}

	while (!state.done) {
		if (cancelled_) {
			// The call is dropped, the daemon cancels a request that nobody asks for by itself
			sd_bus_slot_unref(slot);
			result_ = Result::Cancelled;
			return false;
		}
		r = sd_bus_process(bus_, nullptr);
		if (r < 0) {
			sd_bus_slot_unref(slot);
			result_ = Result::Error;
			howdy_log(LOG_ERR, "Face: processing the bus failed: %s", strerror(-r));
			return false;
		}
		if (r == 0 && !state.done) {
			r = sd_bus_wait(bus_, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(POLL_INTERVAL).count()));
			if (r < 0 && r != -EINTR) {
				sd_bus_slot_unref(slot);
				result_ = Result::Error;
				howdy_log(LOG_ERR, "Face: waiting for the bus failed: %s", strerror(-r));
				return false;
			}
		}
	}
	sd_bus_slot_unref(slot);

	if (state.failed) {
		if (state.error_name == "org.howdy.Authentication.Busy") {
			result_ = Result::Busy;
		} else if (state.error_name == "org.howdy.Authentication.InvalidUser") {
			result_ = Result::InvalidUser;
		} else {
			// A daemon that is not there is an error like any other if it does not come in time. Only
			// a daemon that was lost during a request is a Disconnected, see process_events().
			result_ = Result::Error;
			daemon_unreachable_ = daemon_unreachable(state.error_name);
			howdy_log(LOG_ERR, "Face: StartAuthentication failed: %s", state.error_message.empty() ? state.error_name.c_str() : state.error_message.c_str());
		}
		return false;
	}

	request_id_ = state.request_id;
	request_started_ = true;
	return true;
}

bool FaceAuthenticator::start_authentication_waiting(std::chrono::steady_clock::time_point deadline) {
	bool announced = false;
	const auto busy_deadline = std::chrono::steady_clock::now() + BUSY_TIMEOUT;
	for (;;) {
		daemon_unreachable_ = false;
		if (start_authentication(deadline)) {
			// A daemon that was reported gone before this request is the one that answered it
			daemon_lost_ = false;
			// What the earlier tries said (Busy, Error) is not the result of this request
			result_ = Result::Unset;
			return true;
		}
		const auto now = std::chrono::steady_clock::now();
		const bool busy = result_ == Result::Busy && now < busy_deadline;
		if ((!daemon_unreachable_ && !busy) || cancelled_ || now >= deadline) {
			return false;
		}
		if (!announced) {
			announced = true;
			howdy_log(LOG_NOTICE, busy ? "Face: the daemon is busy, waiting for it" : "Face: the daemon is not there, waiting for it");
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
		howdy_log(LOG_WARNING, "Face: CancelAuthentication failed: %s", error.message ? error.message : strerror(-r));
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
				howdy_log(LOG_WARNING, "Face: ClearAuthentication failed: %s", error.message ? error.message : strerror(-r));
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
				howdy_log(LOG_ERR, "Face: GetResult failed: %s", error.message ? error.message : strerror(-r));
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
			howdy_log(LOG_NOTICE, "Face: the daemon went away, starting the request again");
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
			howdy_log(
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

void FaceAuthenticator::cancel() noexcept {
	cancelled_ = true;
}

#include "fingerprint_authenticator.hh"

#include "main.hh"
#include "translate.hh"

#include <security/pam_ext.h>
#include <sys/syslog.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <thread>

#define S(msg) translate(msg)

namespace {
constexpr const char* FPRINTD_BUS = "net.reactivated.Fprint";

constexpr const char* FPRINTD_MANAGER = "/net/reactivated/Fprint/Manager";

constexpr const char* FPRINTD_MANAGER_IFACE = "net.reactivated.Fprint.Manager";

constexpr const char* FPRINTD_DEVICE_IFACE = "net.reactivated.Fprint.Device";

constexpr const char* FPRINTD_ERROR_ALREADY_IN_USE = "net.reactivated.Fprint.Error.AlreadyInUse";

constexpr const char* FPRINTD_ERROR_NO_ENROLLED_PRINTS = "net.reactivated.Fprint.Error.NoEnrolledPrints";

constexpr const char* FPRINTD_ERROR_NO_ACTION = "net.reactivated.Fprint.Error.NoActionInProgress";

constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);

// How long a call of fprintd may take (the default of the bus), and the shorter time for the calls that put
// things back, which must not hold the end of the authentication for long
constexpr std::uint64_t CALL_TIMEOUT_USEC = 25ULL * 1000000ULL;
constexpr std::uint64_t PUT_BACK_TIMEOUT_USEC = 5ULL * 1000000ULL;

// What an asynchronous call of fprintd is answered with
struct CallState {
	bool done = false;
	bool failed = false;
	sd_bus_message* reply = nullptr;
	std::string error_name;
	std::string error_message;
};

int on_call_reply(sd_bus_message* reply, void* userdata, sd_bus_error* /*ret_error*/) {
	auto* state = static_cast<CallState*>(userdata);
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

	state->reply = sd_bus_message_ref(reply);
	return 0;
}
} // namespace

// Calls a method of fprintd and waits for the answer in slices, so that cancel() is noticed: a reader that
// is stuck (it is reset when the computer wakes up) can hold a call for the whole time of the call. A
// cancelled call returns -ECANCELED and its answer is dropped. The calls that put things back are not
// cancellable, they have to be made after a cancel too. An error of fprintd is in `error`, the answer in `reply`.
int FprintdAuthenticator::call(
	const char* path, const char* interface, const char* member, std::uint64_t timeout_usec, bool cancellable, sd_bus_message** reply, sd_bus_error* error,
	const char* types, ...) {
	sd_bus_message* message = nullptr;
	int r = sd_bus_message_new_method_call(bus_, &message, FPRINTD_BUS, path, interface, member);
	if (r < 0) {
		return r;
	}

	if (types[0] != '\0') {
		va_list arguments;
		va_start(arguments, types);
		r = sd_bus_message_appendv(message, types, arguments);
		va_end(arguments);
		if (r < 0) {
			sd_bus_message_unref(message);
			return r;
		}
	}

	CallState state;
	sd_bus_slot* slot = nullptr;
	r = sd_bus_call_async(bus_, &slot, message, on_call_reply, &state, timeout_usec);
	sd_bus_message_unref(message);
	if (r < 0) {
		return r;
	}

	while (!state.done) {
		if (cancellable && cancelled_) {
			sd_bus_slot_unref(slot);
			return -ECANCELED;
		}

		r = sd_bus_process(bus_, nullptr);
		if (r < 0) {
			sd_bus_slot_unref(slot);
			return r;
		}

		if (r == 0 && !state.done) {
			r = sd_bus_wait(bus_, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(POLL_INTERVAL).count()));
			if (r < 0 && r != -EINTR) {
				sd_bus_slot_unref(slot);
				return r;
			}
		}
	}
	sd_bus_slot_unref(slot);

	if (state.failed) {
		sd_bus_error_set(error, state.error_name.empty() ? SD_BUS_ERROR_FAILED : state.error_name.c_str(), state.error_message.c_str());
		return -EIO;
	}

	*reply = state.reply;
	return 0;
}

FprintdAuthenticator::FprintdAuthenticator(std::string username, const std::function<int(int, const char*)>& conv_function)
	: username_(std::move(username)), conv_function_(conv_function) {
}

FprintdAuthenticator::~FprintdAuthenticator() {
	cleanup();

	if (bus_) {
		sd_bus_unref(bus_);
		bus_ = nullptr;
	}
}

bool FprintdAuthenticator::connect() {
	const int r = sd_bus_open_system(&bus_);

	if (r < 0) {
		result_ = Result::Disconnected;
		return false;
	}

	/*
	 * Watch the fprintd name itself.
	 *
	 * If fprintd disappears while we are verifying,
	 * authentication must fail rather than waiting forever.
	 */
	return sd_bus_match_signal(
			   bus_, &name_owner_slot_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", fprintd_name_owner_changed,
			   this) >= 0;
}

bool FprintdAuthenticator::get_default_device() {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	const int r = call(FPRINTD_MANAGER, FPRINTD_MANAGER_IFACE, "GetDefaultDevice", CALL_TIMEOUT_USEC, true, &reply, &error, "");

	if (r < 0) {
		sd_bus_error_free(&error);
		result_ = r == -ECANCELED ? Result::Cancelled : Result::NoDevice;
		return false;
	}

	const char* path = nullptr;

	const int read_result = sd_bus_message_read(reply, "o", &path);

	if (read_result < 0 || path == nullptr) {
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		result_ = Result::NoDevice;
		return false;
	}

	device_ = path;

	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);

	return true;
}

bool FprintdAuthenticator::claim() {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	const int r = call(device_.c_str(), FPRINTD_DEVICE_IFACE, "Claim", CALL_TIMEOUT_USEC, true, &reply, &error, "s", username_.c_str());

	sd_bus_message_unref(reply);

	if (r < 0) {
		if (r == -ECANCELED) {
			result_ = Result::Cancelled;
		} else if (sd_bus_error_has_name(&error, FPRINTD_ERROR_ALREADY_IN_USE)) {
			result_ = Result::Busy;
		} else {
			result_ = Result::Error;
		}

		sd_bus_error_free(&error);
		return false;
	}

	sd_bus_error_free(&error);

	claimed_ = true;

	return true;
}

bool FprintdAuthenticator::get_scan_type() {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	int r = call(device_.c_str(), "org.freedesktop.DBus.Properties", "Get", CALL_TIMEOUT_USEC, true, &reply, &error, "ss", FPRINTD_DEVICE_IFACE, "scan-type");

	if (r < 0) {
		if (r == -ECANCELED) {
			result_ = Result::Cancelled;
		} else if (sd_bus_error_has_name(&error, FPRINTD_ERROR_ALREADY_IN_USE)) {
			result_ = Result::Busy;
		} else {
			result_ = Result::Error;
		}

		sd_bus_error_free(&error);
		return false;
	}

	sd_bus_error_free(&error);

	// A scan type that can not be read is a press (the usual one), it is no reason to stop
	const char* scan_type = nullptr;
	r = sd_bus_message_enter_container(reply, 'v', "s");
	if (r >= 0) {
		r = sd_bus_message_read(reply, "s", &scan_type);
	}
	if (r >= 0 && scan_type != nullptr && str_equal(scan_type, "swipe")) {
		is_swipe_ = true;
	}

	sd_bus_message_unref(reply);

	return true;
}

bool FprintdAuthenticator::verify_init() {
	/*
	 * Install signal handlers BEFORE VerifyStart().
	 *
	 * This is important because VerifyStatus may arrive very
	 * quickly for some devices.
	 */
	int r = sd_bus_match_signal(bus_, &verify_status_slot_, FPRINTD_BUS, device_.c_str(), FPRINTD_DEVICE_IFACE, "VerifyStatus", verify_status, this);

	return r > -1;
}

bool FprintdAuthenticator::verify_start() {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	const int r = call(device_.c_str(), FPRINTD_DEVICE_IFACE, "VerifyStart", CALL_TIMEOUT_USEC, true, &reply, &error, "s", "any");

	sd_bus_message_unref(reply);

	if (r < 0) {
		if (r == -ECANCELED) {
			// The call is dropped, but fprintd may have started the verification: VerifyStop has to put it back
			verify_started_ = true;
			result_ = Result::Cancelled;
		} else if (sd_bus_error_has_name(&error, FPRINTD_ERROR_NO_ENROLLED_PRINTS)) {

			result_ = Result::NoEnrolledPrints;
		} else if (sd_bus_error_has_name(&error, FPRINTD_ERROR_ALREADY_IN_USE)) {

			result_ = Result::Busy;
		} else {
			result_ = Result::Error;
		}

		sd_bus_error_free(&error);
		return false;
	}

	sd_bus_error_free(&error);

	verify_started_ = true;

	return true;
}

void FprintdAuthenticator::process_events(std::chrono::steady_clock::time_point deadline) {
	int r;
	while (!finished_ && !cancelled_) {
		/*
		 * Process events which are already queued.
		 */
		while (!finished_ && !cancelled_) {
			r = sd_bus_process(bus_, nullptr);

			if (r < 0) {
				result_ = Result::Disconnected;
				return;
			}

			if (r == 0) {
				break;
			}
		}

		if (finished_ || cancelled_) {
			break;
		}

		const auto now = std::chrono::steady_clock::now();

		if (now >= deadline) {
			result_ = Result::Timeout;
			return;
		}

		// In slices, so that cancel() is noticed: nothing on the bus wakes the thread up for it
		const auto remaining = std::min<std::chrono::microseconds>(std::chrono::duration_cast<std::chrono::microseconds>(deadline - now), POLL_INTERVAL);

		/*
		 * sd_bus_wait() expects microseconds.
		 */
		r = sd_bus_wait(bus_, static_cast<std::uint64_t>(remaining.count()));

		if (r < 0) {
			if (r == -EINTR) {
				continue;
			}

			result_ = Result::Disconnected;
			return;
		}
	}

	if (cancelled_) {
		result_ = Result::Cancelled;
	}
}

FprintdAuthenticator::Result
FprintdAuthenticator::authenticate(std::chrono::milliseconds timeout, bool detection_notice, const std::function<void()>& on_scanning, int max_tries) {
	result_ = Result::Error;
	scanning_notified_ = false;

	if (!connect()) {
		cleanup();
		return result_;
	}

	if (!get_default_device()) {
		cleanup();
		return result_;
	}

	if (!claim()) {
		cleanup();
		return result_;
	}

	if (!get_scan_type()) {
		cleanup();
		return result_;
	}

	if (!verify_init()) {
		cleanup();
		return result_;
	}

	const auto deadline = std::chrono::steady_clock::now() + timeout;

	int retries = 0;
	bool no_match = true;
	bool terminated = cancelled_;
	while (retries < max_tries && no_match && !terminated) {
		finished_ = false;
		if (!verify_start()) {
			cleanup();
			return result_;
		}

		if (!scanning_notified_) {
			scanning_notified_ = true;
			if (on_scanning) {
				on_scanning();
			}
		}

		// The first time it is said with the other notices of the module, in their order
		if (detection_notice && retries > 0) {
			conv_function_(PAM_TEXT_INFO, S("Place your finger on the fingerprint reader"));
		}

		process_events(deadline);

		/*
		 * VerifyStop MUST happen before Release.
		 */
		if (verify_started_) {
			verify_stop();
		}

		retries++;
		no_match = (result_ == Result::NoMatch);
		if (no_match && !cancelled_) {
			syslog(
				LOG_ERR,
				"Fingerprint authentication failed "
				"(attempt %d/%d)",
				retries, max_tries);
			if (retries < max_tries) {
				conv_function_(PAM_ERROR_MSG, S("Failed to match fingerprint, please try again"));
			} else {
				conv_function_(PAM_ERROR_MSG, S("Failed to match fingerprint"));
				result_ = Result::MaxTries;
			}
		}

		terminated = (cancelled_ || result_ == Result::Cancelled || result_ == Result::Disconnected);
	}
	release();
	return result_;
}

void FprintdAuthenticator::verify_stop() {
	if (!verify_started_ || !bus_) {
		return;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	const int r = call(device_.c_str(), FPRINTD_DEVICE_IFACE, "VerifyStop", PUT_BACK_TIMEOUT_USEC, false, &reply, &error, "");

	sd_bus_message_unref(reply);

	/*
	 * VerifyStop errors are deliberately ignored here.
	 *
	 * fprintd can already have stopped the verification after
	 * emitting the final VerifyStatus.
	 */
	if (r < 0 && !sd_bus_error_has_name(&error, FPRINTD_ERROR_NO_ACTION)) {
	}

	sd_bus_error_free(&error);

	verify_started_ = false;
}

void FprintdAuthenticator::release() {
	if (!claimed_ || !bus_) {
		return;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;

	call(device_.c_str(), FPRINTD_DEVICE_IFACE, "Release", PUT_BACK_TIMEOUT_USEC, false, &reply, &error, "");

	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);

	claimed_ = false;
}

void FprintdAuthenticator::cleanup() {
	if (!bus_) {
		return;
	}

	if (verify_started_) {
		verify_stop();
	}

	if (claimed_) {
		release();
	}

	sd_bus_slot_unref(verify_status_slot_);
	verify_status_slot_ = nullptr;

	sd_bus_slot_unref(name_owner_slot_);
	name_owner_slot_ = nullptr;
}

int FprintdAuthenticator::verify_status(sd_bus_message* message, void* userdata, sd_bus_error* /*unused*/) {
	auto* self = static_cast<FprintdAuthenticator*>(userdata);

	const char* status = nullptr;
	int done = 0;

	const int r = sd_bus_message_read(message, "sb", &status, &done);

	if (r < 0 || status == nullptr) {
		self->result_ = Result::Error;
		self->finished_ = true;
		return 0;
	}

	/*
	 * Intermediate status.
	 *
	 * fprintd may emit several status messages while
	 * verification is still active.
	 */
	if (!done) {
		if (str_equal(status, "verify-retry-scan")) {
			if (self->is_swipe_) {
				self->conv_function_(PAM_TEXT_INFO, "Swipe your finger again");
			} else {
				self->conv_function_(PAM_TEXT_INFO, "Place your finger on the reader again");
			}
		} else if (str_equal(status, "verify-finger-not-centered")) {
			if (self->is_swipe_) {
				self->conv_function_(PAM_TEXT_INFO, "Your finger was not centered, try swiping your finger again");
			} else {
				self->conv_function_(PAM_TEXT_INFO, "Your finger was not centered, try touching the sensor again");
			}
		} else if (str_equal(status, "verify-remove-and-retry")) {
			if (self->is_swipe_) {
				self->conv_function_(PAM_TEXT_INFO, "Remove your finger, and try swiping your finger again");
			} else {
				self->conv_function_(PAM_TEXT_INFO, "Remove your finger, and try touching the sensor again");
			}
		} else if (str_equal(status, "verify-swipe-too-short")) {
			self->conv_function_(PAM_TEXT_INFO, "Swipe was too short, try again");
		} else if (str_equal(status, "verify-too-fast")) {
			if (self->is_swipe_) {
				self->conv_function_(PAM_TEXT_INFO, "Swipe was too fast, try again");
			} else {
				self->conv_function_(PAM_TEXT_INFO, "Finger scan was too fast, try again");
			}
		}
		return 0;
	}

	if (str_equal(status, "verify-match")) {
		self->result_ = Result::Success;
	} else if (str_equal(status, "verify-no-match")) {
		self->result_ = Result::NoMatch;
	} else if (str_equal(status, "verify-disconnected")) {
		self->result_ = Result::Disconnected;
	} else {
		// verify-unknown-error and everything else
		self->result_ = Result::Error;
	}

	self->finished_ = true;

	return 0;
}

int FprintdAuthenticator::fprintd_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*unused*/) {
	auto* self = static_cast<FprintdAuthenticator*>(userdata);

	const char* name = nullptr;
	const char* old_owner = nullptr;
	const char* new_owner = nullptr;

	if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) < 0) {
		return 0;
	}

	if (!str_equal(name, FPRINTD_BUS)) {
		return 0;
	}

	/*
	 * fprintd disappeared, or was replaced by a new one (it was restarted before it was noticed): the
	 * verification does not go on there either.
	 */
	if (old_owner != nullptr && old_owner[0] != '\0') {
		self->result_ = FprintdAuthenticator::Result::Disconnected;
		self->finished_ = true;
	}

	return 0;
}

void FprintdAuthenticator::cancel() noexcept {
	cancelled_ = true;
}

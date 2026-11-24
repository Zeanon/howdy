#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include <security/pam_modules.h>
#include <systemd/sd-bus.h>

class FaceAuthenticator {
  public:
	enum class Result {
		Success,
		NoMatch,
		NoEnrolledFace,
		MaxTries,
		Timeout,
		TooDark,
		Cancelled,
		Error,
		UnknownRequest,
		NotInitialized,
		Disconnected,
		InvalidUser,
		NoDevice,
		Busy,
		Unset
	};

	explicit FaceAuthenticator(std::string username, std::vector<char const*> args);
	~FaceAuthenticator();

	FaceAuthenticator(const FaceAuthenticator&) = delete;
	FaceAuthenticator& operator=(const FaceAuthenticator&) = delete;

	// on_scanning is called once, when the daemon has found a face model of the user
	// and starts to look for the face, not for a user without a model
	Result authenticate(const std::function<int(int, const char*)>& conv_function, const std::function<void()>& on_scanning, int max_tries);

	void cancel() noexcept;

  private:
	bool connect();
	// Waits for the answer until the deadline (at least two seconds)
	bool start_authentication(std::chrono::steady_clock::time_point deadline);
	// Like start_authentication(), but a daemon that is not there (it restarts, it is started by the bus)
	// is waited for until the deadline, so that a short absence is not a failed authentication
	bool start_authentication_waiting(std::chrono::steady_clock::time_point deadline);
	void cancel_request();
	void process_events(const std::function<void()>& on_scanning);
	void cleanup();

	static Result map_status(const char* status) noexcept;

  private:
	std::string username_;
	std::string request_id_;
	const std::vector<char const*> args_;

	sd_bus* bus_ = nullptr;

	// Set by cancel() from another thread, read in the loops that wait
	std::atomic<bool> cancelled_{false};
	// The daemon went away while a request was running (GetResult could not be answered)
	bool daemon_lost_ = false;
	// start_authentication() failed because the daemon could not be reached, not because it said no
	bool daemon_unreachable_ = false;
	bool request_started_ = false;
	bool scanning_notified_ = false;

	Result result_ = Result::Unset;
};

inline int face_auth_result_to_pam_result(const FaceAuthenticator::Result& result) noexcept {
	switch (result) {
		case FaceAuthenticator::Result::Success: return PAM_SUCCESS;
		case FaceAuthenticator::Result::Busy:
		case FaceAuthenticator::Result::Unset:
		case FaceAuthenticator::Result::NoDevice:
		case FaceAuthenticator::Result::NoEnrolledFace: return PAM_AUTHINFO_UNAVAIL;
		case FaceAuthenticator::Result::NoMatch:
		case FaceAuthenticator::Result::Timeout:
		case FaceAuthenticator::Result::TooDark: return PAM_AUTH_ERR;
		case FaceAuthenticator::Result::MaxTries: return PAM_MAXTRIES;
		case FaceAuthenticator::Result::InvalidUser: return PAM_USER_UNKNOWN;
		case FaceAuthenticator::Result::Error:
		case FaceAuthenticator::Result::Cancelled:
		case FaceAuthenticator::Result::Disconnected:
		case FaceAuthenticator::Result::NotInitialized:
		case FaceAuthenticator::Result::UnknownRequest:
		default: return PAM_CONV_ERR;
	}
}

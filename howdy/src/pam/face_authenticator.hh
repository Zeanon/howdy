#pragma once

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

	Result authenticate(const std::function<int(int, const char*)>& conv_function, bool detection_notice, int max_tries);

	void cancel() noexcept;

  private:
	bool connect();
	bool start_authentication();
	void cancel_request();
	void process_events();
	void cleanup();

	static Result map_status(const char* status) noexcept;

	static int howdy_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);

  private:
	std::string username_;
	std::string request_id_;
	const std::vector<char const*> args_;

	sd_bus* bus_ = nullptr;
	sd_bus_slot* name_owner_slot_ = nullptr;

	bool cancelled_ = false;
	bool request_started_ = false;

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

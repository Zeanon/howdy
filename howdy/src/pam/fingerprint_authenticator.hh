#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include <security/pam_modules.h>
#include <systemd/sd-bus.h>

class FprintdAuthenticator {
  public:
	enum class Result : std::uint_fast8_t { Success, NoMatch, NoEnrolledPrints, MaxTries, Timeout, Cancelled, Error, Disconnected, NoDevice, Busy, Unset };

	FprintdAuthenticator(std::string username, const std::function<int(int, const char*)>& conv_function);
	~FprintdAuthenticator();

	FprintdAuthenticator(const FprintdAuthenticator&) = delete;
	FprintdAuthenticator& operator=(const FprintdAuthenticator&) = delete;

	// on_scanning is called once, when fprintd waits for the finger, not for a user without
	// enrolled fingerprints or without a reader
	Result authenticate(std::chrono::milliseconds timeout, bool detection_notice, const std::function<void()>& on_scanning, int max_tries);

	void cancel() noexcept;

	const std::string& device() const noexcept {
		return device_;
	}

  private:
	bool connect();
	bool get_default_device();

	bool claim();
	bool get_scan_type();
	bool verify_init();
	bool verify_start();

	void verify_stop();
	void release();

	// Calls a method of fprintd, see the definition
	int call(
		const char* path, const char* interface, const char* member, std::uint64_t timeout_usec, bool cancellable, sd_bus_message** reply, sd_bus_error* error,
		const char* types, ...);

	void process_events(std::chrono::steady_clock::time_point deadline);

	void cleanup();

	static int verify_status(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);

	static int fprintd_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);

  private:
	std::string username_;
	const std::function<int(int, const char*)> conv_function_;

	sd_bus* bus_ = nullptr;

	sd_bus_slot* verify_status_slot_ = nullptr;
	sd_bus_slot* name_owner_slot_ = nullptr;

	std::string device_;

	bool claimed_ = false;
	bool is_swipe_ = false;
	bool verify_started_ = false;
	// VerifyStop did not answer in time: the reader is stuck, nothing else is asked of it (the connection
	// that is closed lets fprintd release it)
	bool put_back_stuck_ = false;
	bool scanning_notified_ = false;

	// Set by cancel() from another thread, read in the loops that wait
	std::atomic<bool> cancelled_{false};
	bool finished_ = false;
	// fprintd went away or was replaced at some time of the authentication
	bool owner_replaced_ = false;

	Result result_ = Result::Unset;
};

inline const int fingerprint_result_to_pam_result(const FprintdAuthenticator::Result& result) noexcept {
	switch (result) {
		case FprintdAuthenticator::Result::Success: return PAM_SUCCESS;
		case FprintdAuthenticator::Result::Busy:
		case FprintdAuthenticator::Result::Unset:
		case FprintdAuthenticator::Result::NoDevice:
		case FprintdAuthenticator::Result::NoEnrolledPrints: return PAM_AUTHINFO_UNAVAIL;
		case FprintdAuthenticator::Result::NoMatch:
		case FprintdAuthenticator::Result::Timeout: return PAM_AUTH_ERR;
		case FprintdAuthenticator::Result::MaxTries: return PAM_MAXTRIES;
		case FprintdAuthenticator::Result::Error:
		case FprintdAuthenticator::Result::Cancelled:
		case FprintdAuthenticator::Result::Disconnected:
		default: return PAM_CONV_ERR;
	}
}

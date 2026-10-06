#pragma once

#include <chrono>
#include <string>
#include <functional>

#include <systemd/sd-bus.h>


class FprintdAuthenticator {
public:
	enum class Result : std::uint_fast8_t {
		Success,
		NoDevice,
		Busy,
		NoEnrolledPrints,
		NoMatch,
		MaxTries,
		Timeout,
		Cancelled,
		Disconnected,
		Error,
		Unset
	};

	FprintdAuthenticator(
		std::string username);

	~FprintdAuthenticator();

	FprintdAuthenticator(const FprintdAuthenticator&) = delete;
	FprintdAuthenticator& operator=(const FprintdAuthenticator&) = delete;

	/*
	 * Blocking operation.
	 *
	 * IMPORTANT:
	 * This does NOT create a thread.
	 * Run it inside your existing optional_task.
	 */
	Result authenticate(
		const std::function<int(int, const char *)> &conv_function,
		std::chrono::milliseconds timeout,
		bool detection_notice,
		int max_tries
	);

	/*
	 * Can be called from the controlling Howdy thread.
	 *
	 * The actual D-Bus operation is still performed by
	 * the thread running authenticate().
	 */
	void cancel();

	const std::string& device() const noexcept {
		return device_;
	}

private:
	bool connect();
	bool get_default_device();

	bool claim();
	bool verify_init();
	bool verify_start();

	void verify_stop();
	void release();

	bool process_events(
		std::chrono::steady_clock::time_point deadline);

	void cleanup();

	static int verify_status(
		sd_bus_message* message,
		void* userdata,
		sd_bus_error* ret_error);

	static int verify_finger_selected(
		sd_bus_message* message,
		void* userdata,
		sd_bus_error* ret_error);

	static int fprintd_name_owner_changed(
		sd_bus_message* message,
		void* userdata,
		sd_bus_error* ret_error);

private:
	std::string username_;

	sd_bus* bus_ = nullptr;

	sd_bus_slot* verify_status_slot_ = nullptr;
	sd_bus_slot* verify_finger_selected_slot_ = nullptr;
	sd_bus_slot* name_owner_slot_ = nullptr;

	std::string device_;

	bool claimed_ = false;
	bool verify_started_ = false;

	bool cancelled_ = false;
	bool finished_ = false;

	Result result_ = Result::Error;
};

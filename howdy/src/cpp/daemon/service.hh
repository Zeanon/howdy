#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "authenticator.hh"

namespace howdy {

// Emits the popup signals. sd-bus objects must only be used from the event
// loop thread, so signals from the worker are queued and flushed through an
// eventfd on the loop.
class PopupSignals final : public PopupSink {
  public:
	PopupSignals(sd_bus* bus, sd_event* event);
	~PopupSignals() override;

	PopupSignals(const PopupSignals&) = delete;
	PopupSignals& operator=(const PopupSignals&) = delete;
	PopupSignals(PopupSignals&&) = delete;
	PopupSignals& operator=(PopupSignals&&) = delete;

	void show(std::uint32_t uid, const std::string& message) override;
	void update_message(std::uint32_t uid, const std::string& message) override;
	void update_subtext(std::uint32_t uid, const std::string& subtext) override;
	void close(std::uint32_t uid) override;

  private:
	struct Signal {
		const char* member;
		std::uint32_t uid;
		std::optional<std::string> text;
	};

	// Queue a signal, texts that the popup already shows are not sent again
	void post(Signal signal);
	void flush();
	static int on_wakeup(sd_event_source* source, int fd, std::uint32_t revents, void* userdata);

	sd_bus* bus_;
	sd_bus_slot* slot_ = nullptr;
	sd_event_source* source_ = nullptr;
	int eventfd_ = -1;
	std::mutex mutex_;
	std::deque<Signal> queue_;

	// The texts the popup of a user shows, every signal is a D-Bus message
	struct Shown {
		std::optional<std::string> message;
		std::optional<std::string> subtext;
	};
	std::map<std::uint32_t, Shown> last_state_;
};

// org.boltgolt.howdy.Authentication
class AuthenticationService {
  public:
	AuthenticationService(sd_bus* bus, sd_event* event, Authenticator& authenticator);
	~AuthenticationService();

	AuthenticationService(const AuthenticationService&) = delete;
	AuthenticationService& operator=(const AuthenticationService&) = delete;
	AuthenticationService(AuthenticationService&&) = delete;
	AuthenticationService& operator=(AuthenticationService&&) = delete;

	// Cancel a running authentication and stop the worker
	void shutdown();

  private:
	struct Request {
		std::string username;
		std::vector<std::string> args;
		Status status = Status::Running;
		RequestControl control;
		std::optional<Clock::time_point> finished;
	};

	static int method_start(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int method_get_result(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int method_cancel(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int method_clear(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int on_cleanup_timer(sd_event_source* source, std::uint64_t usec, void* userdata);

	bool clear(const std::string& request_id);
	void worker_loop();
	void purge_finished();

	Authenticator& authenticator_;
	sd_bus_slot* slot_ = nullptr;
	sd_event_source* cleanup_timer_ = nullptr;

	std::mutex mutex_;
	std::condition_variable wakeup_;
	std::map<std::string, std::shared_ptr<Request>> requests_;
	std::optional<std::string> active_request_;
	std::shared_ptr<Request> pending_;
	bool stopping_ = false;
	std::thread worker_;
};

} // namespace howdy

#include "service.hh"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <systemd/sd-id128.h>

#include "caller.hh"
#include "paths.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr auto CLEANUP_AFTER = std::chrono::seconds(30);
constexpr std::uint64_t CLEANUP_INTERVAL_USEC = 5ULL * 1000000ULL;
constexpr auto INITIAL_KEEPALIVE = std::chrono::seconds(2);
constexpr auto KEEPALIVE_EXTENSION = std::chrono::seconds(1);

void check(int r, const char* what) {
	if (r < 0) {
		throw std::runtime_error(std::string(what) + ": " + std::strerror(-r));
	}
}

std::string new_request_id() {
	sd_id128_t id;
	if (sd_id128_randomize(&id) < 0) {
		throw std::runtime_error("Could not generate a request id");
	}
	// sd_id128_randomize() generates a v4 UUID, format it like uuid.uuid4()
	std::array<char, 37> buffer{};
	const auto& b = id.bytes;
	std::snprintf(
		buffer.data(), buffer.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return {buffer.data()};
}

const sd_bus_vtable POPUP_VTABLE[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_SIGNAL_WITH_NAMES("Show", "us", SD_BUS_PARAM(target_uid) SD_BUS_PARAM(message), 0),
	SD_BUS_SIGNAL_WITH_NAMES("UpdateMessage", "us", SD_BUS_PARAM(target_uid) SD_BUS_PARAM(message), 0),
	SD_BUS_SIGNAL_WITH_NAMES("UpdateSubtext", "us", SD_BUS_PARAM(target_uid) SD_BUS_PARAM(subtext), 0),
	SD_BUS_SIGNAL_WITH_NAMES("Close", "u", SD_BUS_PARAM(target_uid), 0),
	SD_BUS_VTABLE_END,
};

} // namespace

// -----------------------------------------------------------------------------
// PopupSignals
// -----------------------------------------------------------------------------

PopupSignals::PopupSignals(sd_bus* bus, sd_event* event) : bus_(bus) {
	check(sd_bus_add_object_vtable(bus_, &slot_, paths::POPUP_PATH, paths::POPUP_INTERFACE, POPUP_VTABLE, this), "Failed to export the popup interface");

	eventfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (eventfd_ < 0) {
		throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
	}
	check(sd_event_add_io(event, &source_, eventfd_, EPOLLIN, on_wakeup, this), "Failed to watch the eventfd");
}

PopupSignals::~PopupSignals() {
	sd_event_source_disable_unref(source_);
	sd_bus_slot_unref(slot_);
	if (eventfd_ >= 0) {
		::close(eventfd_);
	}
}

void PopupSignals::show(std::uint32_t uid, const std::string& message) {
	post({"Show", uid, message});
}

void PopupSignals::update_message(std::uint32_t uid, const std::string& message) {
	post({"UpdateMessage", uid, message});
}

void PopupSignals::update_subtext(std::uint32_t uid, const std::string& subtext) {
	post({"UpdateSubtext", uid, subtext});
}

void PopupSignals::close(std::uint32_t uid) {
	post({"Close", uid, std::nullopt});
}

void PopupSignals::post(Signal signal) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);

		// The popup of a user starts with the text of Show and no subtext
		const std::string_view member = signal.member;
		if (member == "Show") {
			last_state_[signal.uid] = Shown{signal.text, std::nullopt};
		} else if (member == "Close") {
			last_state_.erase(signal.uid);
		} else if (member == "UpdateMessage" || member == "UpdateSubtext") {
			auto& text = member == "UpdateMessage" ? last_state_[signal.uid].message : last_state_[signal.uid].subtext;
			if (text == signal.text) {
				return;
			}
			text = signal.text;
		}

		queue_.push_back(std::move(signal));
	}
	const std::uint64_t one = 1;
	// Failing to wake up only delays the popup, which is fine
	[[maybe_unused]] const auto written = ::write(eventfd_, &one, sizeof(one));
}

void PopupSignals::flush() {
	std::deque<Signal> signals;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		signals.swap(queue_);
	}

	for (const auto& signal : signals) {
		int r = 0;
		if (signal.text.has_value()) {
			r = sd_bus_emit_signal(bus_, paths::POPUP_PATH, paths::POPUP_INTERFACE, signal.member, "us", signal.uid, signal.text->c_str());
		} else {
			r = sd_bus_emit_signal(bus_, paths::POPUP_PATH, paths::POPUP_INTERFACE, signal.member, "u", signal.uid);
		}
		if (r < 0) {
			log(LOG_WARNING, std::string("Failed to emit popup signal ") + signal.member + ": " + std::strerror(-r));
		}
	}
}

int PopupSignals::on_wakeup(sd_event_source* /*source*/, int fd, std::uint32_t /*revents*/, void* userdata) {
	std::uint64_t counter = 0;
	[[maybe_unused]] const auto read_bytes = ::read(fd, &counter, sizeof(counter));
	static_cast<PopupSignals*>(userdata)->flush();
	return 0;
}

// -----------------------------------------------------------------------------
// AuthenticationService
// -----------------------------------------------------------------------------

AuthenticationService::AuthenticationService(sd_bus* bus, sd_event* event, Authenticator& authenticator) : authenticator_(authenticator) {
	// The methods are restricted by the D-Bus policy, so sd-bus must not
	// additionally require privileges from the callers (the PAM module runs
	// with the privileges of e.g. the screen locker).
	static const sd_bus_vtable VTABLE[] = {
		SD_BUS_VTABLE_START(0),
		SD_BUS_METHOD_WITH_NAMES(
			"StartAuthentication", "sas", SD_BUS_PARAM(username) SD_BUS_PARAM(args), "s", SD_BUS_PARAM(request_id), method_start, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD_WITH_NAMES("GetResult", "s", SD_BUS_PARAM(request_id), "s", SD_BUS_PARAM(status), method_get_result, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD_WITH_NAMES(
			"CancelAuthentication", "s", SD_BUS_PARAM(request_id), "b", SD_BUS_PARAM(cancelled), method_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD_WITH_NAMES("ClearAuthentication", "s", SD_BUS_PARAM(request_id), "b", SD_BUS_PARAM(cleared), method_clear, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_VTABLE_END,
	};

	check(
		sd_bus_add_object_vtable(bus, &slot_, paths::AUTHENTICATION_PATH, paths::AUTHENTICATION_INTERFACE, VTABLE, this),
		"Failed to export the authentication interface");

	check(
		sd_event_add_time_relative(event, &cleanup_timer_, CLOCK_MONOTONIC, CLEANUP_INTERVAL_USEC, 0, on_cleanup_timer, this),
		"Failed to add the cleanup timer");
	check(sd_event_source_set_enabled(cleanup_timer_, SD_EVENT_ON), "Failed to enable the cleanup timer");

	worker_ = std::thread([this] { worker_loop(); });
}

AuthenticationService::~AuthenticationService() {
	shutdown();
	sd_event_source_disable_unref(cleanup_timer_);
	sd_bus_slot_unref(slot_);
}

void AuthenticationService::shutdown() {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		for (auto& [id, request] : requests_) {
			request->control.cancel.store(true);
		}
	}
	wakeup_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
}

void AuthenticationService::worker_loop() {
	for (;;) {
		std::shared_ptr<Request> request;
		std::string request_id;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wakeup_.wait(lock, [this] { return stopping_ || pending_ != nullptr; });
			if (stopping_) {
				return;
			}
			request = std::move(pending_);
			pending_.reset();
			request_id = active_request_.value_or("");
		}

		Status result = Status::Error;
		try {
			result = authenticator_.authenticate(request->username, request->args, request->control);
		} catch (const std::exception& exc) {
			log(LOG_ERR, std::string("Authentication worker failed: ") + exc.what());
		} catch (...) {
			log(LOG_ERR, "Authentication worker failed");
		}

		const std::lock_guard<std::mutex> lock(mutex_);
		// Cancellation takes precedence over a late result.
		request->status = request->control.cancel.load() ? Status::Cancelled : result;
		const auto finished = Clock::now();
		request->finished = finished;
		if (!is_privileged(request->owner_uid, true) && authenticator_.start_cooldown().count() > 0) {
			cooldown_until_[request->owner_uid] = finished + authenticator_.start_cooldown();
		}
		if (active_request_ == request_id) {
			active_request_.reset();
		}
	}
}

void AuthenticationService::purge_finished() {
	const auto now = Clock::now();
	const std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = requests_.begin(); it != requests_.end();) {
		const auto& finished = it->second->finished;
		if (finished.has_value() && now - *finished >= CLEANUP_AFTER) {
			it = requests_.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = cooldown_until_.begin(); it != cooldown_until_.end();) {
		it = it->second <= now ? cooldown_until_.erase(it) : std::next(it);
	}
}

bool AuthenticationService::clear(const std::string& request_id, uid_t caller_uid) {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto it = requests_.find(request_id);
	if (it == requests_.end() || !may_access(caller_uid, it->second->owner_uid, true)) {
		return false;
	}
	if (it->second->status == Status::Running) {
		it->second->status = Status::Cancelling;
		it->second->control.cancel.store(true);
	}
	requests_.erase(it);
	return true;
}

int AuthenticationService::method_start(sd_bus_message* message, void* userdata, sd_bus_error* ret_error) {
	auto* self = static_cast<AuthenticationService*>(userdata);

	const char* username = nullptr;
	int r = sd_bus_message_read(message, "s", &username);
	if (r < 0) {
		return r;
	}

	char** strv = nullptr;
	r = sd_bus_message_read_strv(message, &strv);
	if (r < 0) {
		return r;
	}
	std::vector<std::string> args;
	for (char** s = strv; s != nullptr && *s != nullptr; ++s) {
		args.emplace_back(*s);
		std::free(*s); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
	}
	std::free(static_cast<void*>(strv)); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,bugprone-multi-level-implicit-pointer-conversion)

	const auto target_uid = lookup_uid(username);
	if (!target_uid.has_value()) {
		return sd_bus_error_set(ret_error, "org.howdy.Authentication.InvalidUser", "Unknown user");
	}

	uid_t caller_uid = 0;
	r = query_caller_uid(message, caller_uid);
	if (r < 0) {
		return r;
	}
	const bool trusted = is_privileged(caller_uid, true);
	if (!trusted && *target_uid != caller_uid) {
		return sd_bus_error_set(ret_error, "org.howdy.Authentication.NotAuthorized", "Only the own user can be authenticated");
	}

	std::string request_id;
	{
		const std::lock_guard<std::mutex> lock(self->mutex_);
		if (self->active_request_.has_value() || self->stopping_) {
			return sd_bus_error_set(ret_error, "org.howdy.Authentication.Busy", "An authentication is already running");
		}
		if (!trusted) {
			const auto cooldown = self->cooldown_until_.find(caller_uid);
			if (cooldown != self->cooldown_until_.end() && Clock::now() < cooldown->second) {
				return sd_bus_error_set(ret_error, "org.howdy.Authentication.Busy", "Too many authentications, try again in a moment");
			}
		}

		request_id = new_request_id();
		auto request = std::make_shared<Request>();
		request->owner_uid = caller_uid;
		request->username = username;
		request->args = std::move(args);
		request->control.extend_keepalive(INITIAL_KEEPALIVE);

		self->requests_[request_id] = request;
		self->active_request_ = request_id;
		self->pending_ = std::move(request);
	}
	self->wakeup_.notify_one();

	return sd_bus_reply_method_return(message, "s", request_id.c_str());
}

int AuthenticationService::method_get_result(sd_bus_message* message, void* userdata, sd_bus_error* ret_error) {
	auto* self = static_cast<AuthenticationService*>(userdata);

	const char* request_id = nullptr;
	const int r = sd_bus_message_read(message, "s", &request_id);
	if (r < 0) {
		return r;
	}

	uid_t caller_uid = 0;
	if (const int q = query_caller_uid(message, caller_uid); q < 0) {
		return q;
	}

	Status status = Status::Error;
	{
		const std::lock_guard<std::mutex> lock(self->mutex_);
		const auto it = self->requests_.find(request_id);
		// A request of somebody else does not exist for the caller
		if (it == self->requests_.end() || !may_access(caller_uid, it->second->owner_uid, true)) {
			return sd_bus_error_set(ret_error, "org.howdy.Authentication.UnknownRequest", "Unknown authentication request");
		}
		it->second->control.extend_keepalive(KEEPALIVE_EXTENSION);
		status = it->second->status;
	}

	syslog(LOG_DEBUG, "GetResult - request_id: %s, status: %s", request_id, to_string(status));
	return sd_bus_reply_method_return(message, "s", to_string(status));
}

int AuthenticationService::method_cancel(sd_bus_message* message, void* userdata, sd_bus_error* /*ret_error*/) {
	auto* self = static_cast<AuthenticationService*>(userdata);

	const char* request_id = nullptr;
	const int r = sd_bus_message_read(message, "s", &request_id);
	if (r < 0) {
		return r;
	}

	uid_t caller_uid = 0;
	if (const int q = query_caller_uid(message, caller_uid); q < 0) {
		return q;
	}

	bool cancelled = false;
	{
		const std::lock_guard<std::mutex> lock(self->mutex_);
		const auto it = self->requests_.find(request_id);
		if (it != self->requests_.end() && it->second->status == Status::Running && may_access(caller_uid, it->second->owner_uid, true)) {
			it->second->status = Status::Cancelling;
			it->second->control.cancel.store(true);
			cancelled = true;
		}
	}
	return sd_bus_reply_method_return(message, "b", static_cast<int>(cancelled));
}

int AuthenticationService::method_clear(sd_bus_message* message, void* userdata, sd_bus_error* /*ret_error*/) {
	auto* self = static_cast<AuthenticationService*>(userdata);

	const char* request_id = nullptr;
	const int r = sd_bus_message_read(message, "s", &request_id);
	if (r < 0) {
		return r;
	}
	uid_t caller_uid = 0;
	if (const int q = query_caller_uid(message, caller_uid); q < 0) {
		return q;
	}
	return sd_bus_reply_method_return(message, "b", static_cast<int>(self->clear(request_id, caller_uid)));
}

int AuthenticationService::on_cleanup_timer(sd_event_source* source, std::uint64_t /*usec*/, void* userdata) {
	static_cast<AuthenticationService*>(userdata)->purge_finished();
	sd_event_source_set_time_relative(source, CLEANUP_INTERVAL_USEC);
	sd_event_source_set_enabled(source, SD_EVENT_ON);
	return 0;
}

} // namespace howdy

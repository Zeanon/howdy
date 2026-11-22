#pragma once

#include <chrono>
#include <map>
#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "model_store.hh"

namespace howdy {

// The management interface, its name is paths::MANAGEMENT_INTERFACE (set by meson)
//
//   Add(model)            Add(user, model)   -> b
//   List()                List(user)         -> s (JSON)
//   Remove(id)            Remove(user, id)   -> b
//   Clear()               Clear(user)        -> b
//   Compare(metric, face) Compare(user, metric, face) -> (b found, d value, s label)
//   KeepAlive()                                       -> b (the authorization for Compare was kept)
//
// Without a user the models of the caller are used. Every call is checked
// against polkit on behalf of the caller. Compare is called for every frame of
// howdy test, so polkit is asked on the first call of a connection only, the
// connection is remembered while it keeps comparing or sends KeepAlive (see the keep-alive times below). The
// metric is "euclidean" or "dot", the face an array of doubles. The service quits after a while
// without requests, it is started by D-Bus activation again.
class ManagementService {
  public:
	ManagementService(sd_bus* bus, sd_event* event, ModelStore store);
	~ManagementService();

	ManagementService(const ManagementService&) = delete;
	ManagementService& operator=(const ManagementService&) = delete;
	ManagementService(ManagementService&&) = delete;
	ManagementService& operator=(ManagementService&&) = delete;

	// Time without requests until the service stops
	static constexpr std::chrono::seconds IDLE_TIMEOUT{30};

	// A keep-alive like the one of the authentication daemon: a connection that polkit authorized for
	// Compare stays authorized for COMPARE_INITIAL_KEEPALIVE, and every Compare or KeepAlive call of
	// the connection extends that to COMPARE_KEEPALIVE_EXTENSION from then. Without a call it is
	// asked again. KeepAlive only extends what polkit allowed before, it allows nothing itself.
	static constexpr std::chrono::seconds COMPARE_INITIAL_KEEPALIVE{2};
	static constexpr std::chrono::seconds COMPARE_KEEPALIVE_EXTENSION{1};

  private:
	struct Request;

	static int on_message(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int on_authorization(sd_bus_message* reply, void* userdata, sd_bus_error* ret_error);
	static int on_idle(sd_event_source* source, std::uint64_t usec, void* userdata);

	int handle(sd_bus_message* message);
	void execute(const Request& request);
	void finish(Request* request);
	void arm_idle_timer();
	// A bus name is never given to a second connection, so it can be remembered safely
	bool compare_authorized(const std::string& sender);
	void remember_compare(const std::string& sender);

	sd_bus* bus_;
	sd_event* event_;
	ModelStore store_;
	std::map<std::string, std::chrono::steady_clock::time_point> compare_authorized_;
	sd_bus_slot* slot_ = nullptr;
	sd_event_source* idle_timer_ = nullptr;
	int pending_ = 0;
	bool stopping_ = false;
};

} // namespace howdy

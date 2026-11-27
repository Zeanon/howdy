#pragma once

#include <chrono>
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
//
// Without a user the models of the caller are used. Every call is checked
// against polkit on behalf of the caller, except Compare: it is only for root (howdy test runs with
// sudo), because the answers say how far a face is from the models, and a program that may ask
// often enough could work out the models from them. The metric is "euclidean" or "dot", the face
// an array of doubles. The service quits after a while without requests, it is started by D-Bus
// activation again. It gives up its name first and still answers what was on the way.
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
	// Time without requests after the name was given up until the process ends
	static constexpr std::chrono::seconds DRAIN_TIMEOUT{1};

  private:
	struct Request;

	static int on_message(sd_bus_message* message, void* userdata, sd_bus_error* ret_error);
	static int on_authorization(sd_bus_message* reply, void* userdata, sd_bus_error* ret_error);
	static int on_idle(sd_event_source* source, std::uint64_t usec, void* userdata);

	int handle(sd_bus_message* message);
	void execute(const Request& request);
	void finish(Request* request);
	void arm_idle_timer();

	sd_bus* bus_;
	sd_event* event_;
	ModelStore store_;
	sd_bus_slot* slot_ = nullptr;
	sd_event_source* idle_timer_ = nullptr;
	int pending_ = 0;
	bool stopping_ = false;
};

} // namespace howdy

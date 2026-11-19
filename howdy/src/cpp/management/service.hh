#pragma once

#include <chrono>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "model_store.hh"

namespace howdy {

// org.boltgolt.howdy.Management
//
//   Add(model)            Add(user, model)   -> b
//   List()                List(user)         -> s (JSON)
//   Remove(id)            Remove(user, id)   -> b
//   Clear()               Clear(user)        -> b
//
// Without a user the models of the caller are used. Every call is checked
// against polkit on behalf of the caller. The service quits after a while
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

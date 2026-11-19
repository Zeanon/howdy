#pragma once

#include <functional>
#include <string>

#include <gio/gio.h>

namespace gtkui {

struct ManagementResult {
	bool ok = false;
	// The daemon did not allow it: polkit said no, or it is somebody else's models
	bool not_authorized = false;
	// What went wrong, for the user
	std::string error;
	// The JSON of the list
	std::string reply;
};

// The client of the management daemon.
//
// All calls go over the same connection to the system bus, so for polkit they
// come from the same process. polkit keeps a password that was entered for a
// process for a few minutes (auth_self_keep), which is why the user is not
// asked again for every action while the window is open. A program that is
// started again for each action would be asked every time.
class ManagementClient {
  public:
	using Callback = std::function<void(const ManagementResult&)>;

	ManagementClient() = default;
	~ManagementClient();

	ManagementClient(const ManagementClient&) = delete;
	ManagementClient& operator=(const ManagementClient&) = delete;
	ManagementClient(ManagementClient&&) = delete;
	ManagementClient& operator=(ManagementClient&&) = delete;

	// The callbacks run in the main loop. The daemon may wait for the user to
	// enter a password, so the calls have a long timeout.
	void list(const std::string& user, Callback callback);
	void add(const std::string& user, const std::string& model_json, Callback callback);
	void remove(const std::string& user, const std::string& id, Callback callback);

  private:
	void call(const char* method, GVariant* parameters, Callback callback);

	GDBusConnection* connection_ = nullptr;
};

} // namespace gtkui

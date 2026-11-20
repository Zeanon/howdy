#pragma once

#include <sys/types.h>
#include <systemd/sd-bus.h>

namespace howdy {

// The uid of the sender of a message, as the bus knows it and not as the
// sender claims. Returns a negative errno value if it could not be determined.
int query_caller_uid(sd_bus_message* message, uid_t& uid);

// Root may do everything. With trust_own_user the user the daemon runs as
// counts like root, for the daemons that do not run as root.
bool is_privileged(uid_t uid, bool trust_own_user = false);

// Whether the caller may act on something of the owner: root on everything,
// everybody else only on their own
bool may_access(uid_t caller_uid, uid_t owner_uid, bool trust_own_user = false);

} // namespace howdy

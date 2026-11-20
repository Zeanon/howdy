#include "caller.hh"

#include <unistd.h>

namespace howdy {

int query_caller_uid(sd_bus_message* message, uid_t& uid) {
	sd_bus_creds* creds = nullptr;
	int r = sd_bus_query_sender_creds(message, SD_BUS_CREDS_EUID, &creds);
	if (r < 0) {
		return r;
	}
	r = sd_bus_creds_get_euid(creds, &uid);
	sd_bus_creds_unref(creds);
	return r;
}

bool is_privileged(uid_t uid, bool trust_own_user) {
	return uid == 0 || (trust_own_user && uid == geteuid());
}

bool may_access(uid_t caller_uid, uid_t owner_uid, bool trust_own_user) {
	return is_privileged(caller_uid, trust_own_user) || caller_uid == owner_uid;
}

} // namespace howdy

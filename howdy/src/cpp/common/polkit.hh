#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <sys/types.h>
#include <systemd/sd-bus.h>

namespace howdy {

// The caller is not allowed to manage the models, or the check failed
class AuthorizationError : public std::runtime_error {
  public:
	using std::runtime_error::runtime_error;
};

// Time the user gets to answer a password prompt of the polkit agent
inline constexpr std::uint64_t POLKIT_TIMEOUT_USEC = 300ULL * 1000000ULL;

// Only root and the user themselves may manage the models of a user.
// Throws AuthorizationError.
void check_caller_may_manage(uid_t caller_uid, const std::string& target_user);

// Build the polkit CheckAuthorization call for the management action on
// behalf of the given bus name. The caller owns the returned message.
sd_bus_message* new_check_authorization_call(sd_bus* bus, const std::string& bus_name);

// Evaluate the reply of the call above. Throws AuthorizationError if the
// action was not authorized or the call failed.
void evaluate_check_authorization_reply(sd_bus_message* reply);

// Blocking check on behalf of this connection: the user check above and the
// polkit authorization. Throws AuthorizationError.
void check_authorization(sd_bus* bus, uid_t caller_uid, const std::string& target_user);

} // namespace howdy

// Test of is_privileged() and may_access(), which decide who may compare faces and manage models. A
// uid that is neither root nor the user of the test must not be let in, also not for the daemon that
// trusts its own user (the daemons of the tests do, the installed one runs as root).
#include <cstdio>

#include <unistd.h>

#include "caller.hh"

namespace {

int failures = 0;

void check(const char* name, bool value, bool expected) {
	const bool ok = value == expected;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		++failures;
	}
}

} // namespace

int main() {
	const uid_t own = geteuid();
	// Neither root nor the own user
	const uid_t other = own == 4242 ? 4243 : 4242;

	check("root is privileged", howdy::is_privileged(0), true);
	check("another user is not privileged", howdy::is_privileged(other), false);
	check("another user is not privileged for a daemon that trusts its own user", howdy::is_privileged(other, true), false);
	check("the own user is privileged if it is trusted", howdy::is_privileged(own, true), true);
	check("the own user is only privileged without trust if it is root", howdy::is_privileged(own), own == 0);

	check("the owner may access", howdy::may_access(other, other), true);
	check("another user may not access", howdy::may_access(other, own, true), false);
	check("root may access everything", howdy::may_access(0, other), true);
	return failures == 0 ? 0 : 1;
}

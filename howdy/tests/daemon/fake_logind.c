/* A stand-in for the two functions of libsystemd that tell if the login session is remote, preloaded
 * into the test of the PAM module. HOWDY_TEST_REMOTE=1 makes the session a remote one, otherwise it is
 * local, whatever the session the test is started from. */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

int sd_pid_get_session(pid_t pid, char** session) {
	(void)pid;
	*session = strdup("test");
	return *session != NULL ? 0 : -1;
}

int sd_session_is_remote(const char* session) {
	const char* value = getenv("HOWDY_TEST_REMOTE");
	(void)session;
	return value != NULL && value[0] == '1';
}

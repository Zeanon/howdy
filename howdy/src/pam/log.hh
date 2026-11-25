#pragma once

#include <cstdarg>
#include <cstdio>
#include <syslog.h>

// Writes a message of the module to the log of the system. The module does not call openlog(): the name and
// the facility it sets belong to the whole program that uses PAM, and stay when the module is done (the lock
// screen would log the messages of other modules under our name). So every message says itself where it is from.
__attribute__((format(printf, 2, 3))) inline void howdy_log(int priority, const char* format, ...) {
	char message[1024];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	syslog(LOG_AUTHPRIV | priority, "pam_howdy: %s", message);
}

#pragma once

#include "face_authenticator.hh"
#include "fingerprint_authenticator.hh"
#include "log.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>

#include <systemd/sd-login.h>

static inline bool str_equal(const char* a, const char* b) {
	if (a == nullptr && b == nullptr) {
		return true;
	}
	if (a == nullptr || b == nullptr) {
		return false;
	}
	return strcmp(a, b) == 0;
}

enum class ConfirmationType : std::uint_fast8_t { Unset, Terminated, Howdy, Fingerprint, Password };

enum class Workaround : std::uint_fast8_t { Off, Input, Native };

enum class Availability : std::uint_fast8_t { Unknown, Available, Unavailable };

// Part of the definition of pam_handle so we can access the authtok to reset it for password auth
struct pam_handle {
	char* authtok;
};

// Parse string to Workaround enum
inline Workaround get_workaround(const std::string& workaround) {
	if (workaround == "input") {
		return Workaround::Input;
	}

	if (workaround == "native") {
		return Workaround::Native;
	}

	return Workaround::Off;
}

inline std::set<std::string> split_string(const std::string& str, const char& delimiter) {
	std::set<std::string> tokens;
	if (str == "") {
		return tokens;
	}
	size_t start = 0;
	size_t end = str.find(delimiter);

	while (end != std::string::npos) {
		tokens.insert(str.substr(start, end - start));
		start = end + 1;
		end = str.find(delimiter, start);
	}

	tokens.insert(str.substr(start));
	return tokens;
}

inline bool str_to_bool(std::string str) {
	std::transform(str.begin(), str.end(), str.begin(), ::tolower);
	return str == "true" || str == "yes" || str == "on" || str == "y" || str == "1";
}

// `setting` is the name of what is read, for the warning in the log
inline std::chrono::seconds str_to_seconds(const std::string_view& str, const char* setting = "timeout") {
	int value = 0;

	const auto* first = str.data();
	const auto* last = first + str.size();

	const auto [ptr, ec] = std::from_chars(first, last, value);

	if (ec != std::errc{}) {
		howdy_log(LOG_WARNING, "The %s '%.*s' is not a number that can be read", setting, static_cast<int>(str.size()), str.data());
		return std::chrono::seconds{0};
	}

	// What the config file documents (and seconds). Everything else is still read as it always was, a value that
	// was refused now could lock somebody out, but it is said in the log
	static constexpr std::array<std::string_view, 17> UNITS{"",		  "s",		 "sec", "secs", "second", "seconds", "m",	"min", "mins",
															"minute", "minutes", "h",	"hour", "hours",  "d",		 "day", "days"};
	const std::string_view unit(ptr, static_cast<size_t>(last - ptr));
	if (std::ranges::find(UNITS, unit) == UNITS.end() || (value < 0 && !(value == -1 && unit.empty()))) {
		howdy_log(
			LOG_WARNING, "The %s '%.*s' is not valid, only -1 or a positive number with s, m, h or d is", setting, static_cast<int>(str.size()), str.data());
	}

	if (ptr == last) {
		return std::chrono::seconds{static_cast<unsigned>(value)};
	}

	switch (*ptr) {
		// (in 64 bits, the hours of many days do not fit into an unsigned)
		case 'd': return std::chrono::hours{std::int64_t{static_cast<unsigned>(value)} * 24};
		case 'h': return std::chrono::hours{static_cast<unsigned>(value)};
		case 'm': return std::chrono::minutes{static_cast<unsigned>(value)};
		default: return std::chrono::seconds{static_cast<unsigned>(value)};
	}
}

// A number of tries has -1 for no limit. Other negative numbers are read like it, as they always were
// (a value that was refused now could lock somebody out), but it is said in the log
inline void check_tries(long value) {
	if (value < -1) {
		howdy_log(LOG_WARNING, "The number of tries '%ld' is not valid, only -1 or a positive number is", value);
	}
}

// A number of tries of an option of the module, for each of `targets`. One that can not be read leaves them as
// they were and is said in the log, the exception of std::stoi() would end the program that uses PAM.
inline void read_tries(const std::string& str, std::initializer_list<unsigned int*> targets) {
	try {
		const int value = std::stoi(str);
		check_tries(value);
		for (auto* tries : targets) {
			*tries = static_cast<unsigned>(value);
		}
	} catch (const std::logic_error&) {
		howdy_log(LOG_WARNING, "The number of tries '%s' is not a number that can be read", str.c_str());
	}
}

// The limit of the tries for the log, "-1" and other numbers that no int holds are none
inline std::string tries_limit(long long tries) {
	return tries >= INT_MAX ? std::string("no limit") : std::to_string(tries);
}

// A number of tries of the config file
inline unsigned int config_tries(long value) {
	check_tries(value);
	return static_cast<unsigned>(value);
}

// The wait after a wrong password
constexpr auto DEFAULT_DELAY = std::chrono::seconds(2);
constexpr auto MAX_DELAY = std::chrono::seconds(60);

/**
 * Like str_to_seconds() for a delay: it has no "-1", a negative one or one that can not be read is the default
 * (a typo does not turn the delay off), and the password is not asked for later than after a minute.
 */
inline std::chrono::seconds str_to_delay(const std::string_view& str, const char* setting) {
	if (str.starts_with('-')) {
		howdy_log(LOG_WARNING, "The %s '%.*s' is negative, the default is used", setting, static_cast<int>(str.size()), str.data());
		return DEFAULT_DELAY;
	}
	int value = 0;
	if (std::from_chars(str.data(), str.data() + str.size(), value).ec != std::errc{}) {
		howdy_log(LOG_WARNING, "The %s '%.*s' is not a number that can be read, the default is used", setting, static_cast<int>(str.size()), str.data());
		return DEFAULT_DELAY;
	}
	const auto delay = str_to_seconds(str, setting);
	if (delay > MAX_DELAY) {
		howdy_log(LOG_WARNING, "The %s '%.*s' is too long, a minute is used", setting, static_cast<int>(str.size()), str.data());
		return MAX_DELAY;
	}
	return delay;
}

/**
 * Check if an environment variable exists either in the environ array or using
 * getenv.
 * @param name The name of the environment variable.
 * @return The value of the environment variable or nullptr if it doesn't exist
 * or environ is nullptr.
 * @note This function was created because `getenv` wasn't working properly in
 * some contexts (like sudo).
 */
inline bool checkenv(const char* name) {
	if (std::getenv(name) != nullptr) {
		return true;
	}

	auto len = strlen(name);

	for (char** env = environ; *env != nullptr; env++) {
		// The whole name, not a longer one that starts like it
		if (strncmp(*env, name, len) == 0 && (*env)[len] == '=') {
			return true;
		}
	}

	return false;
}

/**
 * Whether the authentication is for a remote session (SSH). The login session of logind says it, which the
 * user can not change, unlike the environment: whoever clears SSH_CONNECTION before sudo is still in the
 * remote session. The environment is looked at, too, for the systems without logind and the processes
 * that belong to no session.
 */
inline bool is_remote_session() {
	char* session = nullptr;
	if (sd_pid_get_session(0, &session) >= 0) {
		const int remote = sd_session_is_remote(session);
		free(session); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
		if (remote > 0) {
			return true;
		}
	}

	return checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") || checkenv("SSH_TTY") || checkenv("SSHD_OPTS");
}

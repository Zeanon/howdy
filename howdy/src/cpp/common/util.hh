#pragma once

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <sys/types.h>
#include <syslog.h>

namespace howdy {

// Marker for user visible strings, kept for parity with the Python i18n.S()
// helper. The daemon ships no translation catalog, so this is the identity.
inline const char* S(const char* text) noexcept {
	return text;
}

// Parse a timeout like "4", "4s", "2m", "30min", "1h" or "1d" into seconds.
// Only the first character after the number is interpreted as unit, like the
// Python implementation. Throws std::invalid_argument on malformed input.
long long str_to_seconds(const std::string& value);

bool str_to_bool(const std::string& value);

// Map the user facing certainty (0 - 100) to a dlib descriptor distance.
double probability_to_certainty(double probability);

std::string trim(const std::string& value, const char* chars = " \t\r\n");

std::string to_upper(std::string value);

void log(int priority, const std::string& message);

// Runs a function when the scope is left, however it is left (return or exception)
template <typename Function> class ScopeExit {
  public:
	explicit ScopeExit(Function function) : function_(std::move(function)) {
	}
	~ScopeExit() {
		// A destructor must not throw
		try {
			function_();
		} catch (const std::exception& exc) {
			log(LOG_ERR, std::string("Cleanup failed: ") + exc.what());
		} catch (...) {
			log(LOG_ERR, "Cleanup failed with an unknown exception");
		}
	}

	ScopeExit(const ScopeExit&) = delete;
	ScopeExit& operator=(const ScopeExit&) = delete;
	ScopeExit(ScopeExit&&) = delete;
	ScopeExit& operator=(ScopeExit&&) = delete;

  private:
	Function function_;
};

// User database lookups, std::nullopt for unknown users
std::optional<uid_t> lookup_uid(const std::string& username);
std::optional<std::string> lookup_username(uid_t uid);
std::optional<std::string> lookup_home(const std::string& username);

// Read a whole file, throws std::runtime_error if it can not be read
std::string read_file(const std::filesystem::path& path);

// Replace the file atomically: the content is written to a temporary file in
// the same directory (created with the given mode, optionally owned by
// owner/group) which then replaces the file. Throws std::runtime_error.
void write_file_atomic(
	const std::filesystem::path& path, const std::string& content, mode_t mode, std::optional<uid_t> owner = std::nullopt,
	std::optional<gid_t> group = std::nullopt);

// Format like Python's str(float), e.g. 60.0 or 0.03
std::string format_double(double value);

} // namespace howdy

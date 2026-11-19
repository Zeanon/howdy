#pragma once

#include <optional>
#include <string>

#include <INIReader.h>

namespace howdy {

// Thin wrapper around INIReader mirroring the configparser calls of the
// Python daemon. A missing or unreadable file yields the fallbacks, just like
// ConfigParser.read() silently ignores missing files.
class Config {
  public:
	explicit Config(const std::string& path);

	[[nodiscard]] bool has(const std::string& section, const std::string& key) const;

	[[nodiscard]] std::string get(const std::string& section, const std::string& key, const std::string& fallback = "") const;
	[[nodiscard]] bool get_bool(const std::string& section, const std::string& key, bool fallback) const;
	[[nodiscard]] long get_int(const std::string& section, const std::string& key, long fallback) const;
	[[nodiscard]] double get_float(const std::string& section, const std::string& key, double fallback) const;

	// Unset or empty values yield std::nullopt.
	[[nodiscard]] std::optional<long> get_optional_int(const std::string& section, const std::string& key) const;

	[[nodiscard]] int parse_error() const {
		return reader_.ParseError();
	}

  private:
	INIReader reader_;
};

} // namespace howdy

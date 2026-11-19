#include "config.hh"

#include "util.hh"

namespace howdy {

Config::Config(const std::string& path) : reader_(path) {
}

bool Config::has(const std::string& section, const std::string& key) const {
	return reader_.HasValue(section, key);
}

std::string Config::get(const std::string& section, const std::string& key, const std::string& fallback) const {
	return reader_.Get(section, key, fallback);
}

bool Config::get_bool(const std::string& section, const std::string& key, bool fallback) const {
	return reader_.GetBoolean(section, key, fallback);
}

long Config::get_int(const std::string& section, const std::string& key, long fallback) const {
	return reader_.GetInteger(section, key, fallback);
}

double Config::get_float(const std::string& section, const std::string& key, double fallback) const {
	return reader_.GetReal(section, key, fallback);
}

std::optional<long> Config::get_optional_int(const std::string& section, const std::string& key) const {
	if (!has(section, key) || trim(get(section, key)).empty()) {
		return std::nullopt;
	}
	return get_int(section, key, 0);
}

} // namespace howdy

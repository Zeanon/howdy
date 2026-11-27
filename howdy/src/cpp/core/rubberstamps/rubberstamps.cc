#include "rubberstamps.hh"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "rubberstamps/hotkey.hh"
#include "rubberstamps/nod.hh"
#include "rubberstamps/stamp.hh"
#include "util.hh"

namespace howdy {

namespace {

std::unique_ptr<Stamp> make_stamp(const std::string& type) {
	if (type == "nod") {
		return std::make_unique<NodStamp>();
	}
	if (type == "hotkey") {
		return std::make_unique<HotkeyStamp>();
	}
	return nullptr;
}

// A number that is the whole text, "5x" is none. Throws if it is not one.
template <typename Number> Number whole_number(const std::string& text, Number (*parse)(const std::string&, std::size_t*)) {
	std::size_t used = 0;
	const Number number = parse(text, &used);
	if (used != text.size()) {
		throw std::invalid_argument(text);
	}
	return number;
}

long to_long(const std::string& text) {
	return whole_number<long>(text, [](const std::string& value, std::size_t* used) { return std::stol(value, used); });
}

double to_double(const std::string& text) {
	return whole_number<double>(text, [](const std::string& value, std::size_t* used) { return std::stod(value, used); });
}

// The seconds of a timeout, a number with s, m or h behind it (5s, 1min) or without a unit.
// std::nullopt for anything else: a unit that is not known must not be read as seconds.
std::optional<double> parse_timeout(const std::string& text) {
	static const std::map<std::string, double> UNITS{{"", 1},		  {"s", 1},	   {"sec", 1},	   {"secs", 1},	   {"second", 1},
													 {"seconds", 1},  {"m", 60},   {"min", 60},	   {"mins", 60},   {"minute", 60},
													 {"minutes", 60}, {"h", 3600}, {"hour", 3600}, {"hours", 3600}};

	const auto letter = std::find_if(text.begin(), text.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
	std::string unit(letter, text.end());
	std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const auto factor = UNITS.find(unit);
	if (factor == UNITS.end()) {
		return std::nullopt;
	}
	try {
		return to_double(std::string(text.begin(), letter)) * factor->second;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

// Set the optional arguments of a rule, like  min_distance=8 min_directions=3. false if one of them
// is not written like that, the stamp has no such option or the value is not one of the option.
bool apply_arguments(Stamp& stamp, const StampRule& rule) {
	std::stringstream stream(rule.arguments);
	std::string argument;
	while (stream >> argument) {
		const auto separator = argument.find('=');
		if (separator == std::string::npos) {
			log(LOG_ERR, "Invalid argument for rubberstamp " + rule.type + ": " + argument);
			return false;
		}
		const std::string key = argument.substr(0, separator);
		const std::string value = argument.substr(separator + 1);

		// Only the arguments that the stamp declared
		const auto option = stamp.options.find(key);
		if (option == stamp.options.end()) {
			log(LOG_ERR, "Unknown config option for rubberstamp " + rule.type + ": " + key);
			return false;
		}

		// The declared type decides how the value is read
		try {
			if (std::holds_alternative<long>(option->second)) {
				option->second = to_long(value);
			} else if (std::holds_alternative<double>(option->second)) {
				option->second = to_double(value);
			} else {
				option->second = value;
			}
		} catch (const std::exception&) {
			log(LOG_ERR, std::format("Invalid value for the option {} of rubberstamp {}: {}", key, rule.type, value));
			return false;
		}
	}
	return true;
}

} // namespace

std::optional<StampRule> parse_stamp_rule(const std::string& line) {
	static const std::regex RULE(R"(^(\w+)\s+([\w\.]+)\s+([a-z]+)(.*)?$)", std::regex::icase);

	std::smatch match;
	if (!std::regex_match(line, match, RULE)) {
		return std::nullopt;
	}

	const auto timeout = parse_timeout(match[2].str());
	if (!timeout.has_value()) {
		return std::nullopt;
	}

	StampRule rule;
	rule.type = match[1].str();
	rule.timeout = *timeout;
	rule.failsafe = match[3].str() != "faildeadly";
	rule.arguments = match[4].str();
	return rule;
}

StampResult run_rubberstamps(const StampContext& context) {
	const bool verbose = context.config.get_bool("debug", "verbose_stamps", false);

	std::stringstream rules(context.config.get("rubberstamps", "stamp_rules"));
	std::string line;
	// All rules are read before the first stamp runs: nobody is asked to nod for an authentication
	// that a later line denies anyway
	std::vector<std::pair<std::unique_ptr<Stamp>, StampRule>> stamps;
	while (std::getline(rules, line)) {
		line = trim(line);
		if (line.size() <= 1) {
			continue;
		}

		// A stamp that can't run must not let the authentication pass, also not one with a typo
		const auto rule = parse_stamp_rule(line);
		if (!rule.has_value()) {
			log(LOG_ERR, "Error parsing rubberstamp rule: " + line);
			return StampResult::Denied;
		}

		auto stamp = make_stamp(rule->type);
		if (!stamp) {
			log(LOG_ERR, "Stamp not installed: " + rule->type);
			return StampResult::Denied;
		}
		if (!apply_arguments(*stamp, *rule)) {
			return StampResult::Denied;
		}
		stamps.emplace_back(std::move(stamp), *rule);
	}

	for (const auto& [stamp, rule] : stamps) {
		if (verbose) {
			log(LOG_INFO, "Executing rubberstamp " + rule.type);
		}

		bool result = false;
		try {
			// Don't start a stamp if the authentication was cancelled already
			check_cancelled(context);
			result = stamp->run(context, rule);
		} catch (const StampCancelled&) {
			return StampResult::Cancelled;
		} catch (const std::exception& exc) {
			// A stamp that can't run must not let the authentication pass
			log(LOG_ERR, "Internal error in rubberstamp " + rule.type + ": " + exc.what());
		}

		if (verbose) {
			log(LOG_INFO, "Stamp " + rule.type + " returned: " + (result ? "true" : "false"));
		}

		// Abort the authentication if the stamp returned false
		if (!result) {
			return StampResult::Denied;
		}
	}
	return StampResult::Passed;
}

} // namespace howdy

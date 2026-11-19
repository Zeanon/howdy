#include "rubberstamps.hh"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <regex>
#include <sstream>
#include <variant>

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

// Set the optional arguments of a rule, like  min_distance=8 min_directions=3
void apply_arguments(Stamp& stamp, const StampRule& rule) {
	std::stringstream stream(rule.arguments);
	std::string argument;
	while (stream >> argument) {
		const auto separator = argument.find('=');
		if (separator == std::string::npos) {
			log(LOG_ERR, "Invalid argument for rubberstamp " + rule.type + ": " + argument);
			continue;
		}
		const std::string key = argument.substr(0, separator);
		const std::string value = argument.substr(separator + 1);

		// Only the arguments that the stamp declared
		const auto option = stamp.options.find(key);
		if (option == stamp.options.end()) {
			log(LOG_ERR, "Unknown config option for rubberstamp " + rule.type + ": " + key);
			continue;
		}

		// The declared type decides how the value is read
		try {
			if (std::holds_alternative<long>(option->second)) {
				option->second = std::stol(value);
			} else if (std::holds_alternative<double>(option->second)) {
				option->second = std::stod(value);
			} else {
				option->second = value;
			}
		} catch (const std::exception&) {
			log(LOG_ERR, std::format("Invalid value for the option {} of rubberstamp {}: {}", key, rule.type, value));
		}
	}
}

} // namespace

std::optional<StampRule> parse_stamp_rule(const std::string& line) {
	static const std::regex RULE(R"(^(\w+)\s+([\w\.]+)\s+([a-z]+)(.*)?$)", std::regex::icase);

	std::smatch match;
	if (!std::regex_match(line, match, RULE)) {
		return std::nullopt;
	}

	// The timeout may have a unit that is ignored, like 5s
	std::string timeout_text = match[2].str();
	std::erase_if(timeout_text, [](unsigned char c) { return std::isalpha(c) != 0; });

	StampRule rule;
	rule.type = match[1].str();
	try {
		rule.timeout = std::stod(timeout_text);
	} catch (const std::exception&) {
		return std::nullopt;
	}
	rule.failsafe = match[3].str() != "faildeadly";
	rule.arguments = match[4].str();
	return rule;
}

StampResult run_rubberstamps(const StampContext& context) {
	const bool verbose = context.config.get_bool("debug", "verbose_stamps", false);

	std::stringstream rules(context.config.get("rubberstamps", "stamp_rules"));
	std::string line;
	while (std::getline(rules, line)) {
		line = trim(line);
		if (line.size() <= 1) {
			continue;
		}

		const auto rule = parse_stamp_rule(line);
		if (!rule.has_value()) {
			log(LOG_ERR, "Error parsing rubberstamp rule: " + line);
			continue;
		}

		auto stamp = make_stamp(rule->type);
		if (!stamp) {
			log(LOG_ERR, "Stamp not installed: " + rule->type);
			continue;
		}
		apply_arguments(*stamp, *rule);

		// Don't start a stamp if the authentication was cancelled already
		check_cancelled(context);

		if (verbose) {
			log(LOG_INFO, "Executing rubberstamp " + rule->type);
		}

		bool result = false;
		try {
			result = stamp->run(context, *rule);
		} catch (const StampCancelled&) {
			return StampResult::Cancelled;
		} catch (const std::exception& exc) {
			// A stamp that can't run must not let the authentication pass
			log(LOG_ERR, "Internal error in rubberstamp " + rule->type + ": " + exc.what());
		}

		if (verbose) {
			log(LOG_INFO, "Stamp " + rule->type + " returned: " + (result ? "true" : "false"));
		}

		// Abort the authentication if the stamp returned false
		if (!result) {
			return StampResult::Denied;
		}
	}
	return StampResult::Passed;
}

} // namespace howdy

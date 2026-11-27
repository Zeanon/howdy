#pragma once

#include <map>
#include <string>
#include <variant>

#include "rubberstamps/rubberstamps.hh"

namespace howdy {

// An option that a stamp declares, the type decides how a value of the rule is parsed
using Option = std::variant<long, double, std::string>;
using Options = std::map<std::string, Option>;

// Sleep, but stop early and throw StampCancelled if the authentication is cancelled
void cancelable_sleep(const StampContext& context, std::chrono::milliseconds duration);

// Throw StampCancelled if the authentication was cancelled, and keep the popup alive.
// Call this regularly.
void check_cancelled(const StampContext& context);

// The base of all rubber stamps. To add one, derive from it, declare the options
// with their defaults in the constructor and register it in make_stamp() in
// rubberstamps.cc
class Stamp {
  public:
	Stamp() = default;
	virtual ~Stamp() = default;
	Stamp(const Stamp&) = delete;
	Stamp& operator=(const Stamp&) = delete;
	Stamp(Stamp&&) = delete;
	Stamp& operator=(Stamp&&) = delete;

	// The optional arguments of the stamp with their defaults
	Options options;

	// true lets the authentication continue, false denies it
	virtual bool run(const StampContext& context, const StampRule& rule) = 0;

	[[nodiscard]] long get_int(const std::string& key) const {
		return std::get<long>(options.at(key));
	}
	[[nodiscard]] const std::string& get_string(const std::string& key) const {
		return std::get<std::string>(options.at(key));
	}
};

} // namespace howdy

#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <unistd.h>

#include "cli.hh"
#include "polkit.hh"
#include "util.hh"

namespace howdy::cli {

void say(const Options& options, const std::string& line) {
	if (!options.gui) {
		std::cout << line << '\n';
		std::cout.flush();
	}
}

std::string ask(const std::string& prompt) {
	std::cout << prompt << std::flush;
	std::string answer;
	if (!std::getline(std::cin, answer)) {
		return "";
	}
	return answer;
}

bool confirm(const std::string& prompt) {
	std::string answer = ask(prompt);
	for (auto& c : answer) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return answer == "y";
}

BusPtr open_system_bus() {
	sd_bus* bus = nullptr;
	const int r = sd_bus_open_system(&bus);
	if (r < 0) {
		throw CliError(std::string("Failed to connect to the system bus: ") + std::strerror(-r));
	}
	return BusPtr(bus);
}

void authorize(sd_bus* bus, const Options& options) {
	try {
		check_authorization(bus, geteuid(), options.user);
	} catch (const std::exception& exc) {
		throw CliError(std::string("Authorization failed: ") + exc.what());
	}
}

void require_root(const std::vector<std::string>& original_arguments) {
	if (geteuid() == 0) {
		return;
	}

	std::string command;
	for (const auto& argument : original_arguments) {
		command += " " + argument;
	}
	throw CliError(std::string(S("Please run this command as root:\n")) + "\n\tsudo howdy" + command);
}

} // namespace howdy::cli

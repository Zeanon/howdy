// The howdy command line interface

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "cli.hh"
#include "paths.hh"
#include "util.hh"

namespace {

using howdy::S;
using howdy::cli::CliError;
using howdy::cli::Options;

constexpr std::array<std::string_view, 12> COMMANDS{
	"add", "clear", "config", "disable", "gui", "list", "remove", "rename", "set", "snapshot", "test", "version",
};

const char* const USAGE = "usage: howdy [-U USER] [--plain] [-h] [-y] command [arguments...]";

void print_help() {
	std::cout << USAGE << "\n\n"
			  << S("Command line interface for Howdy face authentication.") << "\n\n"
			  << "positional arguments:\n"
			  << "  command               "
			  << S("The command option to execute, can be one of the following: add, clear, config, disable, gui, list, "
				   "remove, rename, snapshot, set, test or version.")
			  << "\n"
			  << "  arguments             " << S("Optional arguments for the add, disable, remove, rename and set commands.") << "\n\n"
			  << "options:\n"
			  << "  -U USER, --user USER  " << S("Set the user account to use.") << "\n"
			  << "  -y                    " << S("Skip all questions.") << "\n"
			  << "  --plain               " << S("Print machine-friendly output.") << "\n"
			  << "  -h, --help            " << S("Show this help message and exit.") << "\n\n"
			  << S("For support please visit\nhttps://github.com/boltgolt/howdy") << '\n';
}

// Like argparse: print the usage and the error, exit with 2
[[noreturn]] void usage_error(const std::string& message) {
	std::cout.flush();
	std::cerr << USAGE << "\nhowdy: error: " << message << '\n';
	std::exit(2);
}

// The name of the user of the session, like getpass.getuser()
std::string current_user() {
	for (const char* name : {"LOGNAME", "USER", "LNAME", "USERNAME"}) {
		const char* value = std::getenv(name);
		if (value != nullptr && *value != '\0') {
			return value;
		}
	}
	return howdy::lookup_username(getuid()).value_or("");
}

// The long options may be abbreviated as long as that is unambiguous
std::string expand_long_option(const std::string& option) {
	std::string match;
	for (const char* candidate : {"--user", "--plain", "--gui", "--force-onboarding", "--help"}) {
		if (std::string_view(candidate).starts_with(option)) {
			if (std::string_view(candidate) == option) {
				return option;
			}
			if (!match.empty()) {
				usage_error("ambiguous option: " + option);
			}
			match = candidate;
		}
	}
	return match.empty() ? option : match;
}

bool looks_like_negative_number(const std::string& argument) {
	return argument.size() > 1 && argument[0] == '-' && std::isdigit(static_cast<unsigned char>(argument[1])) != 0;
}

Options parse_arguments(const std::vector<std::string>& arguments) {
	Options options;
	std::vector<std::string> positional;
	std::vector<std::string> unrecognized;
	bool only_positional = false;

	const auto take_user = [&](const std::string& value) { options.user = value; };

	for (std::size_t i = 0; i < arguments.size(); ++i) {
		const std::string& argument = arguments[i];

		if (only_positional || argument.size() < 2 || argument[0] != '-' || looks_like_negative_number(argument)) {
			positional.push_back(argument);
			continue;
		}
		if (argument == "--") {
			only_positional = true;
			continue;
		}

		if (argument.starts_with("--")) {
			const std::size_t equals = argument.find('=');
			const std::string name = expand_long_option(argument.substr(0, equals));
			if (name == "--help") {
				print_help();
				std::exit(0);
			} else if (name == "--user") {
				if (equals != std::string::npos) {
					take_user(argument.substr(equals + 1));
				} else if (i + 1 < arguments.size()) {
					take_user(arguments[++i]);
				} else {
					usage_error("argument -U/--user: expected one argument");
				}
			} else if (name == "--plain" && equals == std::string::npos) {
				options.plain = true;
			} else if (name == "--gui" && equals == std::string::npos) {
				options.gui = true;
			} else if (name == "--force-onboarding" && equals == std::string::npos) {
				// For gui: the setup wizard, even though there are models
				options.force_onboarding = true;
			} else {
				unrecognized.push_back(argument);
			}
		} else if (argument == "-h") {
			print_help();
			std::exit(0);
		} else if (argument == "-y") {
			options.yes = true;
		} else if (argument.starts_with("-U")) {
			if (argument.size() > 2) {
				take_user(argument.substr(2));
			} else if (i + 1 < arguments.size()) {
				take_user(arguments[++i]);
			} else {
				usage_error("argument -U/--user: expected one argument");
			}
		} else {
			unrecognized.push_back(argument);
		}
	}

	if (positional.empty()) {
		usage_error("the following arguments are required: command");
	}

	options.command = positional.front();
	if (std::find(COMMANDS.begin(), COMMANDS.end(), std::string_view(options.command)) == COMMANDS.end()) {
		std::string choices;
		for (const auto command : COMMANDS) {
			choices += (choices.empty() ? "'" : ", '") + std::string(command) + "'";
		}
		usage_error("argument command: invalid choice: '" + options.command + "' (choose from " + choices + ")");
	}
	options.arguments.assign(positional.begin() + 1, positional.end());

	if (!unrecognized.empty()) {
		std::string list;
		for (const auto& argument : unrecognized) {
			list += (list.empty() ? "" : " ") + argument;
		}
		usage_error("unrecognized arguments: " + list);
	}

	return options;
}

int dispatch(const Options& options, const std::vector<std::string>& original_arguments) {
	using namespace howdy::cli;

	if (options.command == "add") {
		return run_add(options);
	}
	if (options.command == "clear") {
		return run_clear(options);
	}
	if (options.command == "config") {
		return run_config(options, original_arguments);
	}
	if (options.command == "disable") {
		return run_disable(options, original_arguments);
	}
	if (options.command == "gui") {
		return run_gui(options);
	}
	if (options.command == "list") {
		return run_list(options);
	}
	if (options.command == "remove") {
		return run_remove(options);
	}
	if (options.command == "rename") {
		return run_rename(options);
	}
	if (options.command == "set") {
		return run_set(options, original_arguments);
	}
	if (options.command == "snapshot") {
		return run_snapshot(options);
	}
	if (options.command == "test") {
		return run_test(options);
	}

	std::cout << "Howdy " << howdy::paths::VERSION << '\n';
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	const std::vector<std::string> original_arguments(argv + 1, argv + argc);

	// Started as howdy-gtk, a link to this program: the window
	if (std::filesystem::path(argv[0]).filename() == "howdy-gtk") {
		Options window;
		window.command = "gui";
		window.force_onboarding = std::find(original_arguments.begin(), original_arguments.end(), "--force-onboarding") != original_arguments.end();
		try {
			return howdy::cli::run_gui(window);
		} catch (const std::exception& exc) {
			std::cerr << exc.what() << '\n';
			return 1;
		}
	}

	// Try to get the user from the session
	std::string user = current_user();
	if (user.empty()) {
		std::cerr << S("Could not determine user, please use the --user flag") << '\n';
		return 1;
	}

	// If we only have 1 argument we print the help text
	if (argc < 2) {
		std::cout << S("current active user: ") << user << "\n" << '\n';
		print_help();
		return 0;
	}

	Options options = parse_arguments(original_arguments);

	if (options.user.empty()) {
		// The test command runs as root through pkexec or sudo, but is meant
		// for the user that started it
		if (options.command == "test") {
			const char* sudo_user = std::getenv("SUDO_USER");
			const char* doas_user = std::getenv("DOAS_USER");
			const char* pkexec_uid = std::getenv("PKEXEC_UID");

			std::string pkexec_user;
			if (pkexec_uid != nullptr && *pkexec_uid != '\0') {
				pkexec_user = howdy::lookup_username(static_cast<uid_t>(std::strtoul(pkexec_uid, nullptr, 10))).value_or("");
			}

			for (const std::string& candidate :
				 {std::string(sudo_user != nullptr ? sudo_user : ""), std::string(doas_user != nullptr ? doas_user : ""), pkexec_user, user}) {
				if (!candidate.empty()) {
					user = candidate;
					break;
				}
			}
		}
		options.user = user;
	}

	// Beyond this point the user can't change anymore, if we still have root as user we need to abort
	// The window can be started with sudo, it acts for the user that called it
	const bool root_command = options.command == "config" || options.command == "set" || options.command == "disable" || options.command == "gui";
	if (options.user == "root" && !root_command) {
		std::cerr << S("Can't run howdy commands as root, please run this command with the --user flag") << '\n';
		return 1;
	}

	try {
		return dispatch(options, original_arguments);
	} catch (const CliError& error) {
		std::cout.flush();
		if (error.what()[0] != '\0') {
			std::cerr << error.what() << '\n';
		}
		return error.code();
	} catch (const std::exception& exc) {
		std::cout.flush();
		std::cerr << "Error: " << exc.what() << '\n';
		return 1;
	}
}

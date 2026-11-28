// howdy config, disable and set: edit the config file

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <ranges>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.hh"
#include "config.hh"
#include "paths.hh"
#include "util.hh"

namespace howdy::cli {

namespace {

std::filesystem::path config_path() {
	return std::filesystem::path(paths::CONFIG_DIR) / "config.ini";
}

std::string lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

// Look the program up in PATH, like shutil.which(). Empty if not found.
std::string which(const std::string& program) {
	const auto executable = [](const std::filesystem::path& path) {
		std::error_code ec;
		return std::filesystem::is_regular_file(path, ec) && access(path.c_str(), X_OK) == 0;
	};

	if (program.empty()) {
		return "";
	}
	if (program.find('/') != std::string::npos) {
		return executable(program) ? program : "";
	}

	const char* path_env = std::getenv("PATH");
	std::string directories = path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin";
	for (std::size_t start = 0; start <= directories.size();) {
		std::size_t end = directories.find(':', start);
		if (end == std::string::npos) {
			end = directories.size();
		}
		const std::string directory = directories.substr(start, end - start);
		const auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / program;
		if (executable(candidate)) {
			return candidate.string();
		}
		start = end + 1;
	}
	return "";
}

// Run a program and return its exit code
int run_program(const std::vector<std::string>& arguments) {
	std::vector<char*> argv;
	argv.reserve(arguments.size() + 1);
	for (const auto& argument : arguments) {
		argv.push_back(const_cast<char*>(argument.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
	}
	argv.push_back(nullptr);

	const pid_t pid = fork();
	if (pid < 0) {
		throw CliError(std::string("Failed to start ") + arguments[0] + ": " + std::strerror(errno));
	}
	if (pid == 0) {
		execvp(argv[0], argv.data());
		_exit(127);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Split into lines, the line endings stay part of the lines
std::vector<std::string> split_lines(const std::string& text) {
	std::vector<std::string> lines;
	for (std::size_t start = 0; start < text.size();) {
		const std::size_t end = text.find('\n', start);
		const std::size_t stop = end == std::string::npos ? text.size() : end + 1;
		lines.push_back(text.substr(start, stop - start));
		start = stop;
	}
	return lines;
}

// Write the config file back with the permissions and owner it had
void save_config(const std::vector<std::string>& lines) {
	const auto path = config_path();

	struct stat info{};
	if (stat(path.c_str(), &info) != 0) {
		throw CliError("Could not access " + path.string() + ": " + std::strerror(errno));
	}

	std::string content;
	for (const auto& line : lines) {
		content += line;
	}

	try {
		write_file_atomic(path, content, info.st_mode & 07777, info.st_uid, info.st_gid);
	} catch (const std::exception& exc) {
		throw CliError(exc.what());
	}
}

// The indented lines behind an option belong to its value (stamp_rules has one rule on each), they
// go with the old value. Empty lines and comments between them stay.
void remove_continuation(std::vector<std::string>& lines, std::size_t option_line) {
	for (std::size_t i = option_line + 1; i < lines.size();) {
		const std::string text = trim(lines[i]);
		if (text.empty() || text.front() == '#' || text.front() == ';') {
			++i;
		} else if (std::isspace(static_cast<unsigned char>(lines[i].front())) != 0) {
			lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
		} else {
			break;
		}
	}
}

std::vector<std::string> load_config_lines() {
	try {
		return split_lines(read_file(config_path()));
	} catch (const std::exception& exc) {
		throw CliError(exc.what());
	}
}

} // namespace

void elevate(const std::vector<std::string>& original_arguments) {
	if (geteuid() == 0) {
		return;
	}

	std::array<char, PATH_MAX> self{};
	const ssize_t length = readlink("/proc/self/exe", self.data(), self.size() - 1);
	if (length < 0) {
		throw CliError(std::string("Could not find the howdy executable: ") + std::strerror(errno));
	}

	std::vector<std::string> arguments{std::string(self.data(), static_cast<std::size_t>(length))};
	arguments.insert(arguments.end(), original_arguments.begin(), original_arguments.end());

	// Keep the environment the graphical editors and howdy-gtk need
	std::vector<std::string> env{"env"};
	for (const char* name : {"DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"}) {
		const char* value = std::getenv(name);
		if (value != nullptr) {
			env.push_back(std::string(name) + "=" + value);
		}
	}
	env.emplace_back("sudo");

	std::vector<std::vector<std::string>> commands;
	for (const char* launcher : {"pkexec", "gksudo", "kdesudo"}) {
		std::vector<std::string> command{launcher};
		command.insert(command.end(), env.begin(), env.end());
		command.insert(command.end(), arguments.begin(), arguments.end());
		commands.push_back(std::move(command));
	}
	std::vector<std::string> sudo{"sudo"};
	sudo.insert(sudo.end(), arguments.begin(), arguments.end());
	commands.push_back(std::move(sudo));

	for (const auto& command : commands) {
		std::vector<char*> argv;
		argv.reserve(command.size() + 1);
		for (const auto& argument : command) {
			argv.push_back(const_cast<char*>(argument.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
		}
		argv.push_back(nullptr);

		execvp(argv[0], argv.data());
		// Try the next launcher if this one is not installed
		if (errno != ENOENT || command[0] == "sudo") {
			throw CliError(std::string("Failed to get root privileges with ") + command[0] + ": " + std::strerror(errno));
		}
	}
}

int run_config(const Options& /*options*/, const std::vector<std::string>& original_arguments) {
	// Make sure we run as root
	elevate(original_arguments);
	// This is this far down so running the command for help is always possible
	require_root(original_arguments);

	// Determine the editor to use: the preferred one, nano or vi
	std::string editor;
	const char* preferred = std::getenv("EDITOR");
	if (preferred != nullptr && !which(preferred).empty()) {
		editor = preferred;
	}
	if (editor.empty()) {
		editor = which("nano");
	}
	if (editor.empty()) {
		editor = which("vi");
	}

	if (editor.empty()) {
		std::string reason = S("Error: Could not find a suitable text editor.\n");
		reason += S("Please install 'nano' or 'vi', or set the EDITOR environment variable.\n");
		reason += S("If you are running this command with sudo, try 'sudo -E howdy config' to preserve your EDITOR variable.");
		std::cout << reason << '\n';
		return 0;
	}

	// Let the user know what we're doing
	std::cout << S("Opening config.ini in ") << std::filesystem::path(editor).filename().string() << '\n';

	try {
		run_program({editor, config_path().string()});
	} catch (const std::exception& exc) {
		std::cout << S("Failed to open editor: ") << exc.what() << '\n';
	}
	return 0;
}

int run_disable(const Options& options, const std::vector<std::string>& original_arguments) {
	require_root(original_arguments);

	// Check if enough arguments have been passed
	if (options.arguments.empty()) {
		throw CliError(S("Please add a 0 (enable) or a 1 (disable) as an argument"));
	}

	// Translate the argument to the right string
	const std::string argument = lower(options.arguments[0]);
	std::string out_value;
	if (argument == "1" || argument == "true") {
		out_value = "true";
	} else if (argument == "0" || argument == "false") {
		out_value = "false";
	} else {
		throw CliError(S("Please only use 0 (enable) or 1 (disable) as an argument"));
	}

	const Config config(config_path().string());
	const std::string current = config.get("core", "disabled", "");
	if (current.empty()) {
		throw CliError("Could not find the \"disabled\" option in " + config_path().string());
	}

	// Don't do anything when the state is already the requested one
	if (out_value == current) {
		throw CliError(std::string(S("The disable option has already been set to ")) + out_value);
	}

	// Only replace the line of the disable config, not a comment that says the same
	const std::string search = "disabled = " + current;
	const std::string replacement = "disabled = " + out_value;
	auto lines = load_config_lines();
	bool replaced = false;
	for (auto& line : lines) {
		if (line.starts_with(search)) {
			line.replace(0, search.size(), replacement);
			replaced = true;
		}
	}
	if (!replaced) {
		throw CliError("Could not find \"" + search + "\" in " + config_path().string());
	}
	save_config(lines);

	// Print what we just did
	std::cout << (out_value == "true" ? S("Howdy has been disabled") : S("Howdy has been enabled")) << '\n';
	return 0;
}

int run_set(const Options& options, const std::vector<std::string>& original_arguments) {
	require_root(original_arguments);

	// Check if enough arguments have been passed
	if (options.arguments.size() < 2) {
		std::string reason = S("Please add a setting you would like to change and the value to set it to\n");
		reason += S("For example:\n");
		reason += "\n\thowdy set certainty 3\n";
		throw CliError(reason);
	}

	// Get the name and value from the cli
	const std::string& name = options.arguments[0];
	const std::string& value = options.arguments[1];

	// "section.name" says which one is meant, several sections have an option of the same name
	const std::size_t dot = name.find('.');
	const std::string section = dot == std::string::npos ? "" : name.substr(0, dot);
	const std::string option = dot == std::string::npos ? name : name.substr(dot + 1);

	// The lines of the option and the sections they are in
	auto lines = load_config_lines();
	std::vector<std::size_t> found;
	std::vector<std::string> sections;
	std::string current;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		const std::string& line = lines[i];
		if (line.starts_with("[")) {
			current = line.substr(1, line.find(']') - 1);
		} else if (line.starts_with(option + " ") && (section.empty() || section == current)) {
			found.push_back(i);
			if (std::find(sections.begin(), sections.end(), current) == sections.end()) {
				sections.push_back(current);
			}
		}
	}

	// If we don't have the line it is not in the config file
	if (found.empty()) {
		throw CliError(std::string("Could not find a \"") + name + "\" config option to set");
	}
	if (sections.size() > 1) {
		std::string choices;
		for (const auto& candidate : sections) {
			choices += "\n\thowdy set ";
			choices.append(candidate).append(".").append(option).append(" ").append(value);
		}
		throw CliError("\"" + option + "\" is in several sections, say which one:" + choices);
	}

	// From the last one on, so that removing lines does not move the ones that are still to do
	for (const std::size_t i : std::views::reverse(found)) {
		lines[i] = option;
		lines[i] += " = ";
		lines[i] += value;
		lines[i] += "\n";
		remove_continuation(lines, i);
	}
	save_config(lines);

	std::cout << S("Config option updated") << '\n';
	return 0;
}

} // namespace howdy::cli

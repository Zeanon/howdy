#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <systemd/sd-bus.h>

namespace howdy::cli {

struct Options {
	std::string command;
	// The arguments of add, disable, remove, rename and set
	std::vector<std::string> arguments;
	// The user the command works on
	std::string user;
	// Skip all questions
	bool yes = false;
	// Machine-friendly output
	bool plain = false;
	// Started by a graphical frontend: no chatter on stdout
	bool gui = false;
	// gui only: start the setup wizard even though there are models
	bool force_onboarding = false;
};

// Exit codes, shared with the Python implementation (exit_codes.py)
inline constexpr int EXIT_FAILURE_CODE = 1;
inline constexpr int EXIT_INVALID_DEVICE = 13;
// The user closed the dialog of `add`
inline constexpr int EXIT_CANCELLED = 3;

// Ends the command. A non-empty message is printed to stderr.
class CliError : public std::runtime_error {
  public:
	explicit CliError(const std::string& message, int code = EXIT_FAILURE_CODE) : std::runtime_error(message), code_(code) {
	}
	[[nodiscard]] int code() const {
		return code_;
	}

  private:
	int code_;
};

// The user cancelled, no message needed
class CliCancelled : public CliError {
  public:
	CliCancelled() : CliError("", EXIT_CANCELLED) {
	}
};

// Print a line of progress, unless a graphical frontend runs the command
void say(const Options& options, const std::string& line);

// Print the question and read the answer from stdin, an empty string on EOF
std::string ask(const std::string& prompt);

// The first characters of the text (not bytes), for the label of a model
std::string truncate_characters(const std::string& text, std::size_t limit);

// Ask a yes/no question that defaults to no
bool confirm(const std::string& prompt);

struct BusDeleter {
	void operator()(sd_bus* bus) const {
		sd_bus_flush_close_unref(bus);
	}
};
using BusPtr = std::unique_ptr<sd_bus, BusDeleter>;

BusPtr open_system_bus();

// Check with polkit that the current user may manage the models of the user
// of the command, throws CliError.
void authorize(sd_bus* bus, const Options& options);

// Commands, return the exit code and throw CliError
int run_list(const Options& options);
int run_remove(const Options& options);
int run_rename(const Options& options);
int run_clear(const Options& options);
int run_add(const Options& options);
int run_config(const Options& options, const std::vector<std::string>& original_arguments);
int run_disable(const Options& options, const std::vector<std::string>& original_arguments);
int run_set(const Options& options, const std::vector<std::string>& original_arguments);
int run_snapshot(const Options& options);
int run_test(const Options& options);
int run_gui(const Options& options);

// Re-run this command with root privileges (pkexec, gksudo, kdesudo or sudo)
// if it does not run as root yet. Only returns if the process is root already.
void elevate(const std::vector<std::string>& original_arguments);

// Tell the user to run the command as root and end it, for the commands that
// change the config.
void require_root(const std::vector<std::string>& original_arguments);

} // namespace howdy::cli

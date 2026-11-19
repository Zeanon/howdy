#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gtkui {

// The text of the user interface. The glade files are translated by GTK, this
// marks the strings of the code for the same, there is no catalog yet.
inline const char* S(const char* text) noexcept {
	return text;
}

struct ProcessResult {
	// False if the program could not be started, the error is in `error`
	bool started = false;
	int exit_code = -1;
	std::string output;
	std::string error;

	// stdout and stderr, for messages to the user
	[[nodiscard]] std::string text() const;
};

// Run a program without blocking the user interface. The callback runs in the
// main loop. stdin is closed, so programs never ask questions.
void run_async(const std::vector<std::string>& argv, std::function<void(const ProcessResult&)> callback);

// Run a program and report every line of its output (stdout and stderr
// together) as it comes in, then the exit code (-1 if it could not be started).
void run_streaming(
	const std::vector<std::string>& argv, const std::string& working_directory, std::function<void(const std::string&)> on_line,
	std::function<void(int)> on_done);

// The absolute path of a program found in PATH, the name itself if there is none
std::string find_program(const std::string& name);

// A command that needs root: pkexec asks for it unless this already is root
std::vector<std::string> privileged(std::vector<std::string> argv);

// This program: privileged parts of the setup run it again as root
std::string self_program();

// The user whose face models are managed: the one that runs this program, or
// the one that asked for root with sudo or pkexec
std::string session_user();

} // namespace gtkui

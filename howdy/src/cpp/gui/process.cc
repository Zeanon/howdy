#include "process.hh"

#include <array>
#include <climits>
#include <cstdlib>
#include <memory>

#include <pwd.h>
#include <unistd.h>

#include <gio/gio.h>

namespace gtkui {

namespace {

GSubprocess* spawn(const std::vector<std::string>& argv, int flags, const std::string& working_directory, std::string& error_message) {
	std::vector<const gchar*> args;
	args.reserve(argv.size() + 1);
	for (const auto& argument : argv) {
		args.push_back(argument.c_str());
	}
	args.push_back(nullptr);

	GSubprocessLauncher* launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(flags));
	if (!working_directory.empty()) {
		g_subprocess_launcher_set_cwd(launcher, working_directory.c_str());
	}

	GError* error = nullptr;
	GSubprocess* process = g_subprocess_launcher_spawnv(launcher, args.data(), &error);
	g_object_unref(launcher);

	if (process == nullptr) {
		error_message = error != nullptr ? error->message : "Could not start the program";
		g_clear_error(&error);
	}
	return process;
}

std::string bytes_to_string(GBytes* bytes) {
	if (bytes == nullptr) {
		return "";
	}
	gsize size = 0;
	const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
	return std::string(data != nullptr ? data : "", size);
}

// The text for labels has to be valid UTF-8
std::string valid_utf8(const std::string& text) {
	gchar* valid = g_utf8_make_valid(text.c_str(), static_cast<gssize>(text.size()));
	std::string result = valid;
	g_free(valid);
	return result;
}

struct AsyncCall {
	std::function<void(const ProcessResult&)> callback;
	GSubprocess* process = nullptr;
};

void on_communicated(GObject* /*source*/, GAsyncResult* async_result, gpointer data) {
	const std::unique_ptr<AsyncCall> call(static_cast<AsyncCall*>(data));

	ProcessResult result;
	result.started = true;

	GBytes* out = nullptr;
	GBytes* err = nullptr;
	GError* error = nullptr;
	if (g_subprocess_communicate_finish(call->process, async_result, &out, &err, &error) != 0) {
		result.output = valid_utf8(bytes_to_string(out));
		result.error = valid_utf8(bytes_to_string(err));
		result.exit_code = g_subprocess_get_if_exited(call->process) != 0 ? g_subprocess_get_exit_status(call->process) : -1;
	} else {
		result.error = error != nullptr ? error->message : "The program failed";
	}
	if (out != nullptr) {
		g_bytes_unref(out);
	}
	if (err != nullptr) {
		g_bytes_unref(err);
	}
	g_clear_error(&error);
	g_object_unref(call->process);

	call->callback(result);
}

struct FailedStart {
	std::function<void(const ProcessResult&)> callback;
	ProcessResult result;
};

struct Stream {
	GSubprocess* process = nullptr;
	GDataInputStream* input = nullptr;
	std::function<void(const std::string&)> on_line;
	std::function<void(int)> on_done;
};

void on_exited(GObject* /*source*/, GAsyncResult* async_result, gpointer data) {
	std::unique_ptr<Stream> stream(static_cast<Stream*>(data));
	g_subprocess_wait_finish(stream->process, async_result, nullptr);
	const int code = g_subprocess_get_if_exited(stream->process) != 0 ? g_subprocess_get_exit_status(stream->process) : -1;
	g_object_unref(stream->input);
	g_object_unref(stream->process);
	stream->on_done(code);
}

void read_line(Stream* stream);

void on_line_read(GObject* /*source*/, GAsyncResult* async_result, gpointer data) {
	auto* stream = static_cast<Stream*>(data);

	gsize length = 0;
	GError* error = nullptr;
	gchar* line = g_data_input_stream_read_line_finish(stream->input, async_result, &length, &error);
	g_clear_error(&error);

	if (line == nullptr) {
		// The end of the output, wait for the program to finish
		g_subprocess_wait_async(stream->process, nullptr, on_exited, stream);
		return;
	}

	const std::string text = valid_utf8(std::string(line, length));
	g_free(line);
	stream->on_line(text);
	read_line(stream);
}

void read_line(Stream* stream) {
	g_data_input_stream_read_line_async(stream->input, G_PRIORITY_DEFAULT, nullptr, on_line_read, stream);
}

} // namespace

std::string ProcessResult::text() const {
	std::string text = error;
	if (!text.empty() && !output.empty()) {
		text += "\n";
	}
	return text + output;
}

void run_async(const std::vector<std::string>& argv, std::function<void(const ProcessResult&)> callback) {
	std::string error;
	GSubprocess* process = spawn(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, "", error);

	if (process == nullptr) {
		auto* failed = new FailedStart{std::move(callback), {}};
		failed->result.error = error;
		g_idle_add(
			[](gpointer data) -> gboolean {
				const std::unique_ptr<FailedStart> failed(static_cast<FailedStart*>(data));
				failed->callback(failed->result);
				return G_SOURCE_REMOVE;
			},
			failed);
		return;
	}

	auto* call = new AsyncCall{std::move(callback), process};
	// Nothing is sent, but stdin is closed so the program never waits for input
	GBytes* nothing = g_bytes_new("", 0);
	g_subprocess_communicate_async(process, nothing, nullptr, on_communicated, call);
	g_bytes_unref(nothing);
}

void run_streaming(
	const std::vector<std::string>& argv, const std::string& working_directory, std::function<void(const std::string&)> on_line,
	std::function<void(int)> on_done) {
	std::string error;
	GSubprocess* process =
		spawn(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, working_directory, error);

	if (process == nullptr) {
		on_line(error);
		auto* done = new std::function<void(int)>(std::move(on_done));
		g_idle_add(
			[](gpointer data) -> gboolean {
				const std::unique_ptr<std::function<void(int)>> done(static_cast<std::function<void(int)>*>(data));
				(*done)(-1);
				return G_SOURCE_REMOVE;
			},
			done);
		return;
	}

	// Never ask questions
	g_output_stream_close(g_subprocess_get_stdin_pipe(process), nullptr, nullptr);

	auto* stream = new Stream{process, g_data_input_stream_new(g_subprocess_get_stdout_pipe(process)), std::move(on_line), std::move(on_done)};
	read_line(stream);
}

std::string find_program(const std::string& name) {
	gchar* path = g_find_program_in_path(name.c_str());
	if (path == nullptr) {
		return name;
	}
	std::string result = path;
	g_free(path);
	return result;
}

std::vector<std::string> privileged(std::vector<std::string> argv) {
	if (geteuid() == 0) {
		return argv;
	}

	// pkexec only runs programs by their absolute path
	argv[0] = find_program(argv[0]);
	argv.insert(argv.begin(), find_program("pkexec"));
	return argv;
}

std::string self_program() {
	std::array<char, PATH_MAX> path{};
	const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
	if (length < 0) {
		return "howdy";
	}
	return {path.data(), static_cast<std::size_t>(length)};
}

std::string session_user() {
	// The cli refuses to work for root, use the user that asked for root
	if (geteuid() == 0) {
		if (const char* sudo_user = std::getenv("SUDO_USER"); sudo_user != nullptr && *sudo_user != '\0') {
			return sudo_user;
		}
		if (const char* pkexec_uid = std::getenv("PKEXEC_UID"); pkexec_uid != nullptr && *pkexec_uid != '\0') {
			const passwd* entry = getpwuid(static_cast<uid_t>(std::strtoul(pkexec_uid, nullptr, 10)));
			if (entry != nullptr) {
				return entry->pw_name;
			}
		}
	}
	return g_get_user_name();
}

} // namespace gtkui

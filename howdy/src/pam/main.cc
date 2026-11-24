#include "main.hh"

#include "enter_device.hh"
#include "optional_task.hh"
#include "translate.hh"

#include <INIReader.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <spawn.h>
#include <stdexcept>
#include <termios.h>

#include <vars.hh>

#include <security/pam_modules.h>
#include <sys/stat.h>
#include <sys/syslog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>

#define S(msg) translate(msg)

// Puts the signal mask of the thread back when the authentication is over
class SignalMaskRestorer {
  public:
	explicit SignalMaskRestorer(const sigset_t& mask) : mask_(mask) {
	}
	~SignalMaskRestorer() {
		pthread_sigmask(SIG_SETMASK, &mask_, nullptr);
	}

	SignalMaskRestorer(const SignalMaskRestorer&) = delete;
	SignalMaskRestorer& operator=(const SignalMaskRestorer&) = delete;
	SignalMaskRestorer(SignalMaskRestorer&&) = delete;
	SignalMaskRestorer& operator=(SignalMaskRestorer&&) = delete;

  private:
	sigset_t mask_;
};

// How long the messages about the methods wait for the answer whether they can be used at all
static const auto AVAILABILITY_WAIT = std::chrono::milliseconds(1000);
// The start of the authentication, the wait for the answers of the methods counts from here
static std::chrono::steady_clock::time_point auth_started;

static const auto DEFAULT_ENTER_TIMEOUT = std::chrono::duration<int, std::chrono::milliseconds::period>(500);

termios original_terminal_flags;

std::mutex mutx;
std::condition_variable convar;

std::set<std::string> whitelist;
std::set<std::string> blacklist;

unsigned int max_password_tries;
unsigned int max_face_auth_tries;
unsigned int max_fingerprint_tries;
unsigned int max_enter_tries;

bool quiet;
bool verbose;
bool blank_lines;
bool timeout_notice;
bool detection_notice;
bool no_confirmation;
bool init_message;

std::chrono::seconds global_timeout;

bool face_auth;
std::optional<std::string> face_auth_timeout = std::nullopt;

bool fingerprint;
std::chrono::seconds fingerprint_timeout;

bool password;

std::function<int(int, const char*)> conv_function;

char* username = nullptr;

char* orig_authtok = nullptr;
pam_handle_t* pam_handle = nullptr;
ConfirmationType confirmation_type = ConfirmationType::Unset;
std::optional<EnterDevice> enter_device = std::nullopt;

/**
 * ---------------------------------------------------------------
 * Authentication state
 * ---------------------------------------------------------------
 */
int pam_res = PAM_IGNORE;
bool terminate = false;
// The signal that ended the authentication (SIGINT, SIGTERM or SIGQUIT), 0 if it was not one
std::atomic<int> received_signal{0};

bool howdy_finished = false;
// Whether a method looks for the user (face model, enrolled fingerprints and a reader are
// there) or has said that it can not be used, set by the tasks of the methods
Availability face_availability = Availability::Unknown;
Availability fingerprint_availability = Availability::Unknown;
bool howdy_success = false;

bool fingerprint_finished = false;
bool fingerprint_success = false;

bool password_finished = false;
bool password_success = false;

int howdy_status_code = PAM_AUTH_ERR;
int fingerprint_status_code = PAM_AUTH_ERR;
int password_status_code = PAM_AUTH_ERR;

/**
 * ---------------------------------------------------------------
 * Parallel tasks
 * ---------------------------------------------------------------
 */
optional_task<int>* howdy_task_ptr;
optional_task<int>* pass_task_ptr;
optional_task<int>* fingerprint_task_ptr;
bool timeouted = false;

/**
 * Inspect the status code returned by the compare process
 * @param	status	The status code
 */
inline void howdy_error(int status, bool retry) {
	// If the process has exited
	if (WIFEXITED(status)) {
		// Get the status code returned
		status = WEXITSTATUS(status);

		switch (status) {
			case CompareResult::NoFaceModel:
				if (blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				conv_function(PAM_ERROR_MSG, S(std::format("No face model known for {}", username).c_str()));
				syslog(LOG_NOTICE, "Failure: no face model known");
				break;
			case CompareResult::TimeoutReached:
				if (blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				if (timeout_notice) {
					if (retry) {
						conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached, please try again"));
					} else {
						conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached"));
					}
				}
				syslog(LOG_ERR, "Failure: face authentication timeout reached");
				break;
			case CompareResult::Abort: syslog(LOG_ERR, "Failure: general abort"); break;
			case CompareResult::TooDark:
				if (blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				conv_function(PAM_ERROR_MSG, S("Face detection image too dark"));
				syslog(LOG_ERR, "Failure: image too dark");
				break;
			case CompareResult::InvalidDevice: syslog(LOG_ERR, "Failure: not possible to open camera at configured path"); break;
			default:
				if (blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				conv_function(PAM_ERROR_MSG, std::string(S(" Unknown error: ") + status).c_str());
				syslog(LOG_ERR, "Failure: unknown error %d", status);
		}
	} else if (WIFSIGNALED(status)) {
		status = WTERMSIG(status);
		syslog(LOG_ERR, "Child killed by signal %s (%d)", strsignal(status), status);
	}
}

/**
 * Format the success message if the status is successful or log the error in
 * the other case
 * @param	status	Status code
 * @return			Returns the conversation function return code
 */
inline int howdy_status(int status) {
	if (status != EXIT_SUCCESS) {
		return PAM_AUTH_ERR;
	}

	syslog(LOG_INFO, "Authenticated with face authentication");
	if (!no_confirmation) {
		// Construct confirmation text from i18n string
		if (blank_lines) {
			conv_function(PAM_TEXT_INFO, "");
		}
		conv_function(PAM_TEXT_INFO, S(std::format("Face authenticated as {}", username).c_str()));
	}

	return PAM_SUCCESS;
}

/**
 * What a method says about whether it can be used: waits for the answer until the authentication is
 * done, and with `bounded` until AVAILABILITY_WAIT after the start at the latest (Unknown then).
 */
inline Availability wait_for_answer(bool enabled, const Availability& state, bool bounded) {
	if (!enabled) {
		return Availability::Unavailable;
	}
	std::unique_lock<std::mutex> lock(mutx);
	const auto answered = [&state] { return state != Availability::Unknown || terminate || confirmation_type != ConfirmationType::Unset; };
	if (bounded) {
		convar.wait_until(lock, auth_started + AVAILABILITY_WAIT, answered);
	} else {
		convar.wait(lock, answered);
	}
	return state;
}

/**
 * Whether a method can be used for the user. A method answers at once whether it can (a face model,
 * enrolled fingerprints and a reader are there), only what can be used is announced. Each method is
 * waited for on its own, until AVAILABILITY_WAIT after the start, so that a slow method holds back only
 * the messages that come after its own. A method that does not answer in time is not announced: the
 * face waits for a daemon that is still starting (after the computer woke up), and a user without a face
 * model would be told to look into the camera before the daemon says that there is nothing to look for.
 * The notices of a method come when it answers, so a slow one is announced then.
 */
inline bool method_available(bool enabled, const Availability& state) {
	return wait_for_answer(enabled, state, true) == Availability::Available;
}

inline void print_init_message() {
	const bool face_available = method_available(face_auth, face_availability);
	const bool fingerprint_available = method_available(fingerprint, fingerprint_availability);
	// The wait ends when the authentication is done, a method that did not answer is not announced then
	if (terminate || confirmation_type != ConfirmationType::Unset) {
		return;
	}
	if (face_available && fingerprint_available) {
		conv_function(PAM_TEXT_INFO, S("Place your finger on the fingerprint reader\nOr look directly into the camera"));
	} else if (fingerprint_available) {
		conv_function(PAM_TEXT_INFO, S("Place your finger on the fingerprint reader"));
	} else if (face_available) {
		conv_function(PAM_TEXT_INFO, S("Look directly into the camera"));
	}
}

/**
 * The notices of the methods that can be used, in this order, each one when its own method has answered.
 * A method that has not answered after AVAILABILITY_WAIT is announced after the others, when it does.
 */
inline void print_detection_notices() {
	// The blank lines set the messages of the methods apart from the password prompt. Without a message there
	// is nothing to set apart, and they would come before the prompt, which the password task prints at the
	// same time.
	bool separated = false;
	const auto separate = [&separated] {
		if (!separated && password && blank_lines) {
			conv_function(PAM_TEXT_INFO, "");
			conv_function(PAM_TEXT_INFO, "");
		}
		separated = true;
	};

	// The wait for an answer ends when the authentication is done, a method that did not answer is not announced then
	const auto done = [] { return terminate || confirmation_type != ConfirmationType::Unset; };

	// A blank line between two notices, whichever comes first (a slow method comes last)
	bool announced = false;
	const auto set_apart = [&] {
		separate();
		if (announced && blank_lines) {
			conv_function(PAM_TEXT_INFO, "");
		}
		announced = true;
	};
	const auto announce_face = [&] {
		set_apart();
		if (verbose) {
			conv_function(PAM_TEXT_INFO, S("Facial authentication..."));
		}
		conv_function(PAM_TEXT_INFO, S("Please look directly into the camera"));
	};
	const auto announce_fingerprint = [&] {
		set_apart();
		if (verbose) {
			conv_function(PAM_TEXT_INFO, S("Fingerprint authentication..."));
		}
		// For the first attempt, so that it comes in this order. The authenticator says it for the next ones.
		conv_function(PAM_TEXT_INFO, S("Place your finger on the fingerprint reader"));
	};

	auto face_state = wait_for_answer(face_auth, face_availability, true);
	if (face_state == Availability::Available && !done()) {
		announce_face();
	}
	auto fingerprint_state = wait_for_answer(fingerprint, fingerprint_availability, true);
	if (fingerprint_state == Availability::Available && !done()) {
		announce_fingerprint();
	}

	// A method that has not answered in time (the daemon of the face is still starting after the computer
	// woke up) is announced when it does, if it can be used: without a face model there is nothing to say.
	// This does not wait for good: the method answers when its task ends (Unavailable at the latest, after
	// the 20 seconds the face waits for its daemon), and a success or an end of the authentication ends the wait.
	if (face_state == Availability::Unknown) {
		face_state = wait_for_answer(face_auth, face_availability, false);
		if (face_state == Availability::Available && !done()) {
			announce_face();
		}
	}
	if (fingerprint_state == Availability::Unknown) {
		fingerprint_state = wait_for_answer(fingerprint, fingerprint_availability, false);
		if (fingerprint_state == Availability::Available && !done()) {
			announce_fingerprint();
		}
	}
}

/**
 * Start the authentication process with the selected modules and
 * wait for a result
 */
inline void do_auth() {
	auth_started = std::chrono::steady_clock::now();
	if (fingerprint) {
		fingerprint_task_ptr->activate();
	}

	if (terminate || confirmation_type != ConfirmationType::Unset) {
		return;
	}
	if (face_auth) {
		howdy_task_ptr->activate();
	}

	if (terminate || confirmation_type != ConfirmationType::Unset) {
		return;
	}
	if (password) {
		pass_task_ptr->activate();
	}

	if (terminate || confirmation_type != ConfirmationType::Unset) {
		return;
	}

	if (init_message) {
		print_init_message();
	}

	if (terminate || confirmation_type != ConfirmationType::Unset) {
		return;
	}
	if (detection_notice) {
		print_detection_notices();
	} else if (password && blank_lines && (method_available(face_auth, face_availability) || method_available(fingerprint, fingerprint_availability))) {
		conv_function(PAM_TEXT_INFO, "");
	}

	if (!terminate && confirmation_type == ConfirmationType::Unset) {
		std::unique_lock<std::mutex> lock(mutx);

		timeouted = !convar.wait_for(lock, global_timeout, [&]() -> bool {
			/*
			 * One option authenticated successfully
			 */
			if (confirmation_type != ConfirmationType::Unset) {
				return true;
			}

			/*
			 * If every available authentication method has finished
			 * unsuccessfully, we also need to leave the wait.
			 */
			return (!face_auth || howdy_finished) && (!fingerprint || fingerprint_finished) && (!password || password_finished);
		});
	}
}

/**
 * Workaround function for the password prompt
 * @param	workaround		Workaround method to use
 */
inline void workaround_function(const Workaround& workaround) {
	// We want to stop the password prompt, either by canceling the thread when
	// workaround is set to "native", or by emulating "Enter" input with
	// "input"
	switch (workaround) {
		// UNSAFE: We cancel the thread using pthread, pam_get_authtok seems to be
		// a cancellation point
		case Workaround::Native: pass_task_ptr->stop(true); break;
		case Workaround::Input:
			if (enter_device.has_value()) {
				try {
					unsigned int retries;

					enter_device.value().send_enter_press();
					for (retries = 1; retries < max_enter_tries && password && pass_task_ptr->active() &&
									  pass_task_ptr->wait(DEFAULT_ENTER_TIMEOUT) == std::future_status::timeout;
						 retries++) {
						enter_device.value().send_enter_press();
					}

					if (retries >= max_enter_tries) {
						syslog(LOG_WARNING, "Failed to send enter input before the retries limit");
						conv_function(
							PAM_ERROR_MSG, S("Failed to send Enter press before the "
											 "retries limit, waiting for user to press it instead"));
					}
				} catch (std::runtime_error& err) {
					syslog(LOG_WARNING, "Failed to send enter input: %s", err.what());
					conv_function(
						PAM_ERROR_MSG, S("Failed to send Enter press, waiting for user "
										 "to press it instead"));
				}
			} else {
				syslog(LOG_WARNING, "Insufficient permissions to create the fake device");
				conv_function(
					PAM_ERROR_MSG, S("Insufficient permissions to send Enter "
									 "press, waiting for user to press it instead"));
			}
		// No automated workaround, the user has to press enter themselves
		case Workaround::Off:
			// We stop the thread (will block until the enter key is pressed if the
			// input wasn't focused or if the uinput device failed to send keypress)
			pass_task_ptr->stop(false);
		default: break;
	}

	// reset the terminal flags, so input does not stay hidden for example
	if (password) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal_flags);
	}
	if (orig_authtok != nullptr) {
		pam_handle->authtok = orig_authtok;
	}
}

// The options of the module that the face authentication daemon does not take from the PAM
// configuration any more, they are read from the config file
inline bool is_removed_face_option(const std::string& arg) {
	static constexpr std::array<std::string_view, 11> REMOVED = {
		"face-auth:certainty=",
		"face-auth:anti-spoof-threshold=",
		"face-auth:damo-flir-threshold=",
		"face-auth:micromotion-detection-buffer-size=",
		"face-auth:micromotion-threshold=",
		"face-auth:texture-detection-buffer-size=",
		"face-auth:texture-threshold=",
		"face-auth:similarity-detection-buffer-size=",
		"face-auth:identical-threshold=",
		"face-auth:sudden-change-threshold=",
		"face-auth:confirmations=",
	};
	return std::any_of(REMOVED.begin(), REMOVED.end(), [&](std::string_view option) { return arg.starts_with(option); });
}

// The configuration decides whether and how Howdy authenticates, so it is only used if nobody
// but root can change it. The checks run on the opened file, so it can not be swapped in between.
inline std::optional<std::string> read_trusted_config(const char* path) {
	const int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		syslog(LOG_ERR, "Failed to open the configuration file %s: %s", path, strerror(errno));
		return std::nullopt;
	}

	struct stat info{};
	std::optional<std::string> content;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		syslog(LOG_ERR, "The configuration file %s is not a regular file", path);
	} else if (info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		syslog(LOG_ERR, "Ignoring the configuration file %s: it must belong to root and must not be writable by others", path);
	} else {
		std::string data;
		std::array<char, 4096> buffer{};
		ssize_t count = 0;
		while ((count = read(fd, buffer.data(), buffer.size())) > 0) {
			data.append(buffer.data(), static_cast<size_t>(count));
		}
		if (count < 0) {
			syslog(LOG_ERR, "Failed to read the configuration file %s: %s", path, strerror(errno));
		} else {
			content = std::move(data);
		}
	}
	close(fd);
	return content;
}

/**
 * The main function, runs the identification and authentication
 * @param	pamh			The handle to interface directly with PAM
 * @param	flags			Flags passed on to us by PAM, XORed
 * @param	argc			Amount of rules in the PAM config (disregarded)
 * @param	argv			Options defined in the PAM config
 * @param	ask_auth_tok	True if we should ask for a password too
 * @return					Returns a PAM return code
 */
inline int identify(pam_handle_t* pamh, int flags, int argc, const char** argv, bool ask_auth_tok) { // NOLINT(readability-function-cognitive-complexity)
	openlog("pam_howdy", 0, LOG_AUTHPRIV);

	const auto config_content = read_trusted_config(CONFIG_FILE_PATH);
	if (!config_content.has_value()) {
		return PAM_SYSTEM_ERR;
	}
	const INIReader config(config_content->data(), config_content->size());

	// Error out if we could not read the config file
	if (config.ParseError() != 0) {
		syslog(LOG_ERR, "Failed to parse the configuration file: %d", config.ParseError());
		return PAM_SYSTEM_ERR;
	}

	if (config.GetBoolean("core", "disabled", false)) {
		syslog(LOG_INFO, "Skipped authentication, Howdy is disabled");
		return PAM_IGNORE;
	}

	// get all values from the config
	bool abort_if_ssh = config.GetBoolean("core", "abort_if_ssh", true);
	bool abort_if_lid_closed = config.GetBoolean("core", "abort_if_lid_closed", true);

	quiet = config.GetBoolean("core", "quiet", false);
	verbose = config.GetBoolean("core", "verbose", true);
	blank_lines = config.GetBoolean("core", "blank_lines", true);
	timeout_notice = config.GetBoolean("core", "timeout_notice", true);
	detection_notice = config.GetBoolean("core", "detection_notice", true);
	no_confirmation = config.GetBoolean("core", "no_confirmation", false);
	init_message = config.GetBoolean("core", "init_message", false);

	whitelist = split_string(config.GetString("core", "whitelist", ""), ',');
	blacklist = split_string(config.GetString("core", "blacklist", ""), ',');

	global_timeout = str_to_seconds(config.GetString("core", "timeout", "30min"));

	face_auth = config.GetBoolean("face_authentication", "enable", true);
	max_face_auth_tries = static_cast<unsigned>(config.GetInteger("face_authentication", "max_tries", 1));
	std::optional<std::string> show_auth_popup = std::nullopt;

	fingerprint = config.GetBoolean("fingerprint", "enable", false);
	fingerprint_timeout = str_to_seconds(config.GetString("fingerprint", "timeout", "30s"));
	max_fingerprint_tries = static_cast<unsigned>(config.GetInteger("fingerprint", "max_tries", 3));

	password = config.GetBoolean("password", "enable", false);
	max_password_tries = static_cast<unsigned>(config.GetInteger("password", "max_tries", 3));
	max_enter_tries = static_cast<unsigned>(config.GetInteger("password", "max_enter_tries", 5));
	bool password_nodelay = config.GetBoolean("password", "nodelay", false);
	std::chrono::seconds password_delay = str_to_seconds(config.GetString("password", "delay", "2s"));
	Workaround workaround = get_workaround(config.GetString("core", "workaround", "off"));

	std::vector<const char*> password_args;

	// parse the options passed to the module
	std::string arg;
	for (int i = 0; i < argc; i++) {
		if (argv[i] == nullptr) {
			continue;
		}

		arg = std::string(argv[i]);
		if (arg == "abort-if-ssh") {
			abort_if_ssh = true;
		} else if (arg.starts_with("abort-if-ssh=")) {
			abort_if_ssh = str_to_bool(arg.substr(13, arg.length()));
		} else if (arg == "abort-if-lid-closed") {
			abort_if_lid_closed = true;
		} else if (arg.starts_with("abort-if-lid-closed=")) {
			abort_if_lid_closed = str_to_bool(arg.substr(20, arg.length()));
		} else if (arg == "quiet") {
			quiet = true;
		} else if (arg.starts_with("quiet=")) {
			quiet = str_to_bool(arg.substr(6, arg.length()));
		} else if (arg == "verbose") {
			verbose = true;
		} else if (arg.starts_with("verbose=")) {
			verbose = str_to_bool(arg.substr(8, arg.length()));
		} else if (arg == "blank-lines") {
			blank_lines = true;
		} else if (arg.starts_with("blank-lines=")) {
			blank_lines = str_to_bool(arg.substr(12, arg.length()));
		} else if (arg == "timeout-notice") {
			timeout_notice = true;
		} else if (arg.starts_with("timeout-notice=")) {
			timeout_notice = str_to_bool(arg.substr(15, arg.length()));
		} else if (arg == "detection-notice") {
			detection_notice = true;
		} else if (arg.starts_with("detection-notice=")) {
			detection_notice = str_to_bool(arg.substr(17, arg.length()));
		} else if (arg == "no-confirmation") {
			no_confirmation = true;
		} else if (arg.starts_with("no-confirmation=")) {
			no_confirmation = str_to_bool(arg.substr(17, arg.length()));
		} else if (arg == "face-auth") {
			face_auth = true;
		} else if (arg.starts_with("face-auth=")) {
			face_auth = str_to_bool(arg.substr(12, arg.length()));
		} else if (arg == "fingerprint") {
			fingerprint = true;
		} else if (arg.starts_with("fingerprint=")) {
			fingerprint = str_to_bool(arg.substr(12, arg.length()));
		} else if (arg == "password") {
			password = true;
		} else if (arg.starts_with("password=")) {
			password = str_to_bool(arg.substr(9, arg.length()));
		} else if (arg == "password:nodelay") {
			password_nodelay = true;
		} else if (arg.starts_with("password:nodelay=")) {
			password_nodelay = str_to_bool(arg.substr(17, arg.length()));
		} else if (arg == "init-message") {
			init_message = true;
		} else if (arg.starts_with("init-message=")) {
			init_message = str_to_bool(arg.substr(13, arg.length()));
		} else if (arg == "face-auth:show-auth-popup") {
			show_auth_popup = std::optional<std::string>("show-auth-popup");
		} else if (arg.starts_with("face-auth:show-auth-popup=")) {
			show_auth_popup = std::optional<std::string>(arg.substr(10, arg.length()));
		} else if (is_removed_face_option(arg)) {
			// These settings were passed to the daemon, which only reads them from the config file now
			syslog(LOG_WARNING, "The option '%s' is not supported any more, set it in the config file", arg.c_str());
		} else if (arg.starts_with("global-timeout=")) {
			global_timeout = str_to_seconds(arg.substr(15, arg.length()));
		} else if (arg.starts_with("timeout=")) {
			face_auth_timeout = std::optional<std::string>(arg);
			fingerprint_timeout = str_to_seconds(arg.substr(8, arg.length()));
		} else if (arg.starts_with("face-auth:timeout=")) {
			face_auth_timeout = std::optional<std::string>(arg.substr(10, arg.length()));
		} else if (arg.starts_with("fingerprint:timeout=")) {
			fingerprint_timeout = str_to_seconds(arg.substr(20, arg.length()));
		} else if (arg.starts_with("max-tries=")) {
			max_password_tries = static_cast<unsigned>(std::stoi(arg.substr(10, arg.length())));
			max_face_auth_tries = static_cast<unsigned>(std::stoi(arg.substr(10, arg.length())));
			max_fingerprint_tries = static_cast<unsigned>(std::stoi(arg.substr(10, arg.length())));
		} else if (arg.starts_with("password:max-tries=")) {
			max_password_tries = static_cast<unsigned>(std::stoi(arg.substr(19, arg.length())));
		} else if (arg.starts_with("face-auth:max-tries=")) {
			max_face_auth_tries = static_cast<unsigned>(std::stoi(arg.substr(20, arg.length())));
		} else if (arg.starts_with("fingerprint:max-tries=")) {
			max_fingerprint_tries = static_cast<unsigned>(std::stoi(arg.substr(22, arg.length())));
		} else if (arg.starts_with("password:max-enter-tries=")) {
			max_enter_tries = static_cast<unsigned>(std::stoi(arg.substr(25, arg.length())));
		} else if (arg.starts_with("password:delay=")) {
			password_delay = str_to_seconds(arg.substr(15, arg.length()));
		} else if (arg.starts_with("workaround=")) {
			workaround = get_workaround(arg.substr(11, arg.length()));
		} else if (arg.starts_with("whitelist=")) {
			whitelist = split_string(arg.substr(10, arg.length()), ',');
		} else if (arg.starts_with("blacklist=")) {
			blacklist = split_string(arg.substr(10, arg.length()), ',');
		} else if (arg.starts_with("password:")) {
			password_args.push_back(arg.substr(9, arg.length()).c_str());
		}
	}

	if (password_delay.count() < 1) {
		password_nodelay = true;
	}
	if (password_nodelay) {
		password_args.push_back("nodelay");
	}
	if (quiet) {
		verbose = false;
		blank_lines = false;
		detection_notice = false;
		no_confirmation = true;
	}
	password_args.shrink_to_fit();
	password = password && ask_auth_tok;

	// Range checks for timeouts and max-tries
	if (global_timeout.count() < 1) {
		syslog(LOG_ERR, "Global-timeout needs to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}

	if (fingerprint_timeout.count() < 1) {
		syslog(LOG_ERR, "Timeout for fingerprint needs to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}

	if (max_password_tries < 1) {
		syslog(LOG_ERR, "Max-tries for password need to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}
	if (max_face_auth_tries < 1) {
		syslog(LOG_ERR, "Max-tries for face-auth need to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}
	if (max_fingerprint_tries < 1) {
		syslog(LOG_ERR, "Max-tries for fingerprinr need to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}
	if (max_enter_tries < 1) {
		syslog(LOG_ERR, "Max-tries for enter need to be greater than 0 or -1");
		return PAM_SERVICE_ERR;
	}

	// Save the original terminal flags to they can be reset after password
	tcgetattr(STDIN_FILENO, &original_terminal_flags);

	pam_handle = pamh;
	// Get the username from PAM, needed to match correct face model
	pam_res = pam_get_user(pam_handle, const_cast<const char**>(&username), nullptr);
	if (pam_res != PAM_SUCCESS) {
		syslog(LOG_ERR, "Failed to get username");
		return pam_res;
	}

	// The detection with the environment only, in case the one of the login session does not work:
	/*
	if (abort_if_ssh && (checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") || checkenv("SSH_TTY") || checkenv("SSHD_OPTS"))) {
		syslog(LOG_INFO, "Skipped authentication, SSH session detected");
		return PAM_IGNORE;
	}
	*/

	if (abort_if_ssh && is_remote_session()) {
		syslog(LOG_INFO, "Skipped authentication, remote session detected");
		return PAM_IGNORE;
	}

	if (abort_if_lid_closed) {
		glob_t glob_result;

		// Get any files containing lid state
		int return_value = glob("/proc/acpi/button/lid/*/state", 0, nullptr, &glob_result);

		// GLOB_NOMATCH means there is no lid, which is normal on desktops
		if (return_value != 0 && return_value != GLOB_NOMATCH) {
			syslog(LOG_ERR, "Failed to read files from glob: %d", return_value);
			if (errno != 0) {
				syslog(LOG_ERR, "Underlying error: %s (%d)", strerror(errno), errno);
			}
		} else {
			for (size_t i = 0; i < glob_result.gl_pathc; i++) {
				std::ifstream file(std::string(glob_result.gl_pathv[i]));
				std::string lid_state;
				std::getline(file, lid_state, static_cast<char>(file.eof()));

				if (lid_state.find("closed") != std::string::npos) {
					globfree(&glob_result);

					syslog(LOG_INFO, "Skipped authentication, closed lid detected");
					return PAM_IGNORE;
				}
			}
		}
		globfree(&glob_result);
	}

	if ((!whitelist.empty() && !whitelist.contains(std::string(username))) || blacklist.contains(std::string(username))) {
		return PAM_IGNORE;
	}

	// Will contain PAM conversation structure
	struct pam_conv* conv = nullptr;
	const void** conv_ptr = const_cast<const void**>(reinterpret_cast<void**>(&conv));

	// Retrieve the PAM conversation structure
	pam_res = pam_get_item(pam_handle, PAM_CONV, conv_ptr);
	if (pam_res != PAM_SUCCESS || conv == nullptr || conv->conv == nullptr) {
		syslog(LOG_ERR, "Failed to acquire conversation");
		return PAM_SYSTEM_ERR;
	}

	// Wrap the PAM conversation function in our own, easier function
	conv_function = [conv](int msg_type, const char* msg_str) -> int {
		const struct pam_message msg = {.msg_style = msg_type, .msg = msg_str};
		const struct pam_message* msgp = &msg;

		struct pam_response res = {};
		struct pam_response* resp = &res;

		return conv->conv(1, &msgp, &resp, conv->appdata_ptr);
	};

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGQUIT);

	// Only for the time of the authentication: the thread is not ours, the signals would stay blocked
	// for the rest of its life, and the threads of the tasks (which have to inherit the mask) end here
	sigset_t previous_mask;
	pthread_sigmask(SIG_BLOCK, &set, &previous_mask);
	const SignalMaskRestorer restore_mask(previous_mask);
	/*
	 * -----------------------------------------------------------------------
	 * Dummy process to capture SIGINT/SIGTERM/SIGQUIT to terminate everything
	 * -----------------------------------------------------------------------
	 */
	optional_task<void> signal_task([&] {
		/*
		 * Make the thread asynchronously cancellable.
		 */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

		int sig = 0;
		sigwait(&set, &sig);

		received_signal = sig;
		terminate = true;
		if (confirmation_type == ConfirmationType::Unset) {
			confirmation_type = ConfirmationType::Terminated;
		}
		convar.notify_one();
		return;
	});
	signal_task.activate();

	// The only settings of a face authentication that are passed to the daemon, everything else comes
	// from the config file
	std::vector<char const*> args;
	if (show_auth_popup) {
		args.push_back((*show_auth_popup).data());
	}
	if (face_auth_timeout) {
		args.push_back((*face_auth_timeout).data());
	}

	/*
	 * ---------------------------------------------------------------
	 * Password task
	 * ---------------------------------------------------------------
	 */
	optional_task<int> pass_task([&]() -> int {
		/*
		 * Make the thread asynchronously cancellable.
		 */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

		auto* handle = dlopen(PAM_UNIX_PATH, RTLD_NOW);
		if (!handle) {
			syslog(LOG_ERR, "Could not load %s: %s", PAM_UNIX_PATH, dlerror());

			{
				std::unique_lock<std::mutex> lock(mutx);

				password_finished = true;
				password_status_code = PAM_AUTHINFO_UNAVAIL;
			}

			convar.notify_one();
			return PAM_AUTHINFO_UNAVAIL;
		}
		dlerror();

		int (*password_pam_sm)(pam_handle_t* pam_handle, int flags, int argc, const char** argv);
		*reinterpret_cast<void**>(&password_pam_sm) = dlsym(handle, "pam_sm_authenticate");

		const char* err = dlerror();
		if (err) {
			syslog(LOG_ERR, "Could not load passwort authentication function: %s", err);

			{
				std::unique_lock<std::mutex> lock(mutx);

				password_finished = true;
				password_status_code = PAM_AUTHINFO_UNAVAIL;
			}

			convar.notify_one();
			return PAM_AUTHINFO_UNAVAIL;
		}

		orig_authtok = pam_handle->authtok;

		int rc = 0;
		bool success = false;
		bool keep_running = true;
		bool terminated = terminate;
		unsigned int retries = 0;
		while (retries < max_password_tries && keep_running) {
			rc = password_pam_sm(pam_handle, flags, static_cast<int>(password_args.size()), password_args.data());

			success = (rc == PAM_SUCCESS);
			terminated = (rc == PAM_AUTHTOK_ERR || rc == PAM_CONV_ERR || terminate);
			keep_running = (!success && !terminate && confirmation_type == ConfirmationType::Unset);

			retries++;
			if (keep_running) {
				if (!password_nodelay) {
					std::this_thread::sleep_for(password_delay);
				}
				syslog(
					LOG_ERR,
					"Password authentication failed "
					"(attempt %d/%d)",
					retries, max_face_auth_tries);

				if (retries > 1 && blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				if (retries < max_password_tries) {
					pam_handle->authtok = orig_authtok;
					conv_function(PAM_ERROR_MSG, S("Password authentication failed, please try again"));
				} else {
					conv_function(PAM_ERROR_MSG, S("Password authentication failed"));
				}
			}
		}

		{
			std::unique_lock<std::mutex> lock(mutx);

			password_finished = true;
			password_success = success;
			password_status_code = rc;

			if (confirmation_type == ConfirmationType::Unset) {
				if (success) {
					confirmation_type = ConfirmationType::Password;
				} else if (terminated) {
					confirmation_type = ConfirmationType::Terminated;
				}
			}
		}

		convar.notify_one();
		return rc;
	});
	pass_task_ptr = &pass_task;

	/*
	 * ---------------------------------------------------------------
	 * Howdy task
	 * ---------------------------------------------------------------
	 */
	auto face_authenticator = std::make_shared<FaceAuthenticator>(username, args);
	optional_task<int> howdy_task([&]() -> int {
		/*
		 * Make the thread asynchronously cancellable.
		 */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

		// The daemon looks for the face, so the user has a face model
		const auto on_scanning = [] {
			{
				std::unique_lock<std::mutex> lock(mutx);
				face_availability = Availability::Available;
			}
			convar.notify_one();
		};
		FaceAuthenticator::Result rc = face_authenticator->authenticate(conv_function, on_scanning, static_cast<int>(max_face_auth_tries));

		bool terminated = (rc == FaceAuthenticator::Result::Cancelled || rc == FaceAuthenticator::Result::Disconnected || terminate);

		{
			std::unique_lock<std::mutex> lock(mutx);

			howdy_finished = true;
			// Without a scan there was nothing to look for
			if (face_availability == Availability::Unknown) {
				face_availability = (rc == FaceAuthenticator::Result::Success) ? Availability::Available : Availability::Unavailable;
			}
			howdy_success = (rc == FaceAuthenticator::Result::Success);
			howdy_status_code = face_auth_result_to_pam_result(rc);

			if (confirmation_type == ConfirmationType::Unset) {
				if (howdy_success) {
					confirmation_type = ConfirmationType::Howdy;
				} else if (terminated) {
					confirmation_type = ConfirmationType::Terminated;
				} else if (rc == FaceAuthenticator::Result::Timeout && timeout_notice) {
					conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached"));
				}
			}
		}
		convar.notify_one();
		return howdy_status_code;
	});
	howdy_task_ptr = &howdy_task;

	/*
	 * ---------------------------------------------------------------
	 * Fingerprint task
	 * ---------------------------------------------------------------
	 */
	auto fingerprint_authenticator = std::make_shared<FprintdAuthenticator>(username, conv_function);
	optional_task<int> fingerprint_task([&]() -> int {
		/*
		 * Make the thread asynchronously cancellable.
		 *
		 * authenticate() may block while fprintd waits for the
		 * user's finger.
		 */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

		// fprintd waits for the finger, so the user has enrolled fingerprints and there is a reader
		const auto on_scanning = [] {
			{
				std::unique_lock<std::mutex> lock(mutx);
				fingerprint_availability = Availability::Available;
			}
			convar.notify_one();
		};
		FprintdAuthenticator::Result rc =
			fingerprint_authenticator->authenticate(fingerprint_timeout, detection_notice, on_scanning, static_cast<int>(max_fingerprint_tries));

		bool terminated = (rc == FprintdAuthenticator::Result::Cancelled || rc == FprintdAuthenticator::Result::Disconnected || terminate);

		{
			std::unique_lock<std::mutex> lock(mutx);

			fingerprint_finished = true;
			// Without a scan there was nothing to touch
			if (fingerprint_availability == Availability::Unknown) {
				fingerprint_availability = (rc == FprintdAuthenticator::Result::Success) ? Availability::Available : Availability::Unavailable;
			}
			fingerprint_success = (rc == FprintdAuthenticator::Result::Success);
			fingerprint_status_code = fingerprint_result_to_pam_result(rc);

			if (confirmation_type == ConfirmationType::Unset) {
				if (fingerprint_success) {
					confirmation_type = ConfirmationType::Fingerprint;
				} else if (terminated) {
					confirmation_type = ConfirmationType::Terminated;
				} else if (rc == FprintdAuthenticator::Result::Timeout && timeout_notice) {
					conv_function(PAM_ERROR_MSG, S("Fingerprint authentication timeout reached"));
				}
			}
		}

		convar.notify_one();
		return fingerprint_status_code;
	});
	fingerprint_task_ptr = &fingerprint_task;

	/*
	 * ---------------------------------------------------------------
	 * EnterDevice
	 * ---------------------------------------------------------------
	 */
	if (workaround == Workaround::Input && euidaccess("/dev/uinput", W_OK | R_OK) == 0) {
		enter_device.emplace();
	}

	do_auth();

	signal_task.stop(true);

	face_authenticator->cancel();
	fingerprint_authenticator->cancel();

	switch (confirmation_type) {
		/*
		 * ---------------------------------------------------------------
		 * Fingerprint authenticated first
		 * ---------------------------------------------------------------
		 */
		case ConfirmationType::Fingerprint: {
			/**
			 * terminate all sub tasks
			 */
			howdy_task.stop(true);
			fingerprint_task.stop(false);
			workaround_function(workaround);

			pam_res = fingerprint_task.get();
			if (pam_res == PAM_SUCCESS && !no_confirmation) {
				if (blank_lines) {
					conv_function(PAM_TEXT_INFO, "");
				}
				conv_function(PAM_TEXT_INFO, S(std::format("Fingerprint authenticated as {}", username).c_str()));
				syslog(LOG_INFO, "Authenticated with fingerprint");
			}
			return pam_res;
		}

		/*
		 * ---------------------------------------------------------------
		 * Password authenticated first
		 * ---------------------------------------------------------------
		 */
		case ConfirmationType::Password: {
			/**
			 * terminate all sub tasks
			 */
			pass_task.stop(false);
			howdy_task.stop(true);
			fingerprint_task.stop(false);

			pam_res = pass_task.get();
			if (pam_res == PAM_SUCCESS && !no_confirmation) {
				conv_function(PAM_TEXT_INFO, S(std::format("Password authenticated as {}", username).c_str()));
				syslog(LOG_INFO, "Authenticated with password");
			}
			return pam_res;
		}

		/*
		 * ---------------------------------------------------------------
		 * Howdy authenticated first
		 * ---------------------------------------------------------------
		 */
		case ConfirmationType::Howdy: {
			/**
			 * terminate all sub tasks
			 */
			howdy_task.stop(false);
			fingerprint_task.stop(false);
			workaround_function(workaround);

			return howdy_status(howdy_task.get());
		}

		/*
		 * ---------------------------------------------------------------
		 * Authentication was terminated
		 * ---------------------------------------------------------------
		 */
		case ConfirmationType::Terminated: {
			/**
			 * terminate all sub tasks
			 */
			howdy_task.stop(true);
			fingerprint_task.stop(false);
			workaround_function(Workaround::Native);

			// Give the signal back to the program, the one that was sent: it is delivered when the signals
			// are unblocked again, on the return. A method that is cancelled or lost (fprintd or the daemon of
			// the face went away) is no signal, the program must not get one: it would end the lock screen.
			if (received_signal != 0) {
				raise(received_signal.load());
			}
			return PAM_CONV_ERR;
		}

		default: break;
	}

	/*
	 * ---------------------------------------------------------------
	 * NO AUTHENTICATION SUCCEEDED
	 * ---------------------------------------------------------------
	 */

	/*
	 * At this point all available authentication mechanisms have
	 * finished unsuccessfully.
	 */

	pass_task.stop(timeouted);
	howdy_task.stop(timeouted);
	fingerprint_task.stop(timeouted);

	/*
	 * Prefer the Howdy result for the existing Howdy error messages
	 * when Howdy actually ran to completion.
	 */
	if (howdy_finished) {
		return howdy_status(howdy_task.get());
	}

	if (fingerprint_finished) {
		return fingerprint_status_code;
	}

	if (password_finished) {
		return password_status_code;
	}

	return PAM_AUTH_ERR;
}

// Called by PAM when a user needs to be authenticated, for example by running
// the sudo command
PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc, const char** argv) {
	return identify(pamh, flags, argc, argv, true);
}

// Called by PAM when a session is started, such as by the su command
PAM_EXTERN auto pam_sm_open_session(pam_handle_t* pamh, int flags, int argc, const char** argv) -> int {
	return identify(pamh, flags, argc, argv, false);
}

// The functions below are required by PAM, but not needed in this module
PAM_EXTERN auto pam_sm_acct_mgmt(pam_handle_t* pamh, int flags, int argc, const char** argv) -> int {
	return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_close_session(pam_handle_t* pamh, int flags, int argc, const char** argv) -> int {
	return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_chauthtok(pam_handle_t* pamh, int flags, int argc, const char** argv) -> int {
	return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_setcred(pam_handle_t* pamh, int flags, int argc, const char** argv) -> int {
	return PAM_IGNORE;
}

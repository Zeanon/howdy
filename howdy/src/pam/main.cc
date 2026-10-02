#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <glob.h>
#include <libintl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>
#include <termios.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <tuple>
#include <iostream>

#include <dlfcn.h>

#include <INIReader.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include "enter_device.hh"
#include "main.hh"
#include "optional_task.hh"
#include <paths.hh>

const auto DEFAULT_ENTER_TIMEOUT =
    std::chrono::duration<int, std::chrono::milliseconds::period>(500);

#define S(msg) gettext(msg)

termios original_terminal_flags;

std::mutex mutx;
std::condition_variable convar;

std::set<std::string> whitelist;
std::set<std::string> blacklist;

unsigned int max_face_auth_tries;
unsigned int max_password_tries;
unsigned int max_fingerprint_tries;
unsigned int max_enter_tries;

bool verbose;
bool timeout_notice;
bool detection_notice;
bool no_confirmation;

bool face_unlock;
std::optional<const char*> face_auth_timeout = std::nullopt;

bool fingerprint;
unsigned int fingerprint_timeout;

bool password;

std::function<int(int, const char *)> conv_function;

char *username = nullptr;

ConfirmationType confirmation_type = ConfirmationType::Unset;
std::optional<EnterDevice> enter_device = std::nullopt;

/*
  * ---------------------------------------------------------------
  * Authentication state
  * ---------------------------------------------------------------
  */
int pam_res = PAM_IGNORE;
bool terminate = false;

bool howdy_finished = false;
bool howdy_success = false;

bool fingerprint_finished = false;
bool fingerprint_success = false;

bool password_finished = false;
bool password_success = false;

int howdy_status_code = PAM_AUTH_ERR;
int fingerprint_status_code = PAM_AUTH_ERR;
int password_status_code = PAM_AUTH_ERR;


optional_task<int> *child_task_ptr;
optional_task<int> *pass_task_ptr;
optional_task<int> *fingerprint_task_ptr;


/**
 * Inspect the status code returned by the compare process
 * @param  status        The status code
 * @param  conv_function The PAM conversation function
 */
inline void howdy_error(int status) {
  // If the process has exited
  if (WIFEXITED(status)) {
    // Get the status code returned
    status = WEXITSTATUS(status);

    switch (status) {
    case CompareError::NO_FACE_MODEL:
      syslog(LOG_NOTICE, "Failure: no face model known");
      break;
    case CompareError::TIMEOUT_REACHED:
      if (verbose) {
        conv_function(PAM_TEXT_INFO, "");
      }
      if (timeout_notice) {
        conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached"));
      }
      syslog(LOG_ERR, "Failure: face authentication timeout reached");
      break;
    case CompareError::ABORT:
      syslog(LOG_ERR, "Failure: general abort");
      break;
    case CompareError::TOO_DARK:
      if (verbose) {
        conv_function(PAM_TEXT_INFO, "");
      }
      conv_function(PAM_ERROR_MSG, S("Face detection image too dark"));
      syslog(LOG_ERR, "Failure: image too dark");
      break;
    case CompareError::INVALID_DEVICE:
      syslog(LOG_ERR,
             "Failure: not possible to open camera at configured path");
      break;
    default:
      if (verbose) {
        conv_function(PAM_TEXT_INFO, "");
      }
      conv_function(PAM_ERROR_MSG,
                    std::string(S("Unknown error: ") + status).c_str());
      syslog(LOG_ERR, "Failure: unknown error %d", status);
    }
  } else if (WIFSIGNALED(status)) {
    // We get the signal
    status = WTERMSIG(status);
    syslog(LOG_ERR, "Child killed by signal %s (%d)", strsignal(status),
           status);
  }
}

/**
 * Format the success message if the status is successful or log the error in
 * the other case
 * @param  username      Username
 * @param  status        Status code
 * @param  config        INI  configuration
 * @param  conv_function PAM conversation function
 * @return          Returns the conversation function return code
 */
inline int howdy_status(int status) {
  if (status != EXIT_SUCCESS) {
    return PAM_AUTH_ERR;
  }

  syslog(LOG_INFO, "Authenticated with face authentication");
  if (!no_confirmation) {
    // Construct confirmation text from i18n string
    if (verbose) {
      conv_function(PAM_TEXT_INFO, "");
    }
    conv_function(PAM_TEXT_INFO, S(std::format("Face authenticated as {}", username).c_str()));
    //conv_function(PAM_TEXT_INFO, ""); //TODO
  }

  return PAM_SUCCESS;
}

/**
 * Start the authentication process with the selected modules and
 * wait for a result
 */
inline void do_auth() {
  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (face_unlock) {
    child_task_ptr->activate();
  }

  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (fingerprint) {
    fingerprint_task_ptr->activate();
  }

  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (password) {
    pass_task_ptr->activate();
  }

  usleep(100000);

  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (password && verbose) {
    conv_function(PAM_TEXT_INFO, "");
  }
  if (detection_notice) {
    if (password && verbose) {
      conv_function(PAM_TEXT_INFO, "");
    }
    if (face_unlock) {
      if ((verbose && conv_function(PAM_TEXT_INFO, S("Facial authentication..."))) != PAM_SUCCESS ||
           conv_function(PAM_TEXT_INFO, S("Please look directly into the camera")) != PAM_SUCCESS) {
        syslog(LOG_ERR, "Failed to send detection notice");
      }
      if (verbose) {
        conv_function(PAM_TEXT_INFO, "");
      }
    }
    if (fingerprint && verbose) {
      conv_function(PAM_TEXT_INFO, S("Fingerprint authentication..."));
    }
  }

  if (!terminate && confirmation_type == ConfirmationType::Unset) {
    std::unique_lock<std::mutex> lock(mutx);

    convar.wait(lock, [&] {
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
      return howdy_finished &&
            fingerprint_finished &&
            (!password || password_finished);
    });
  }
}

/**
 * Workaround function for the password prompt
 * @param  workaround    Workaround method to use
 */
inline void workaround_function(const Workaround &workaround) {
  // We want to stop the password prompt, either by canceling the thread when
  // workaround is set to "native", or by emulating "Enter" input with
  // "input"
  switch (workaround) {
    // UNSAFE: We cancel the thread using pthread, pam_get_authtok seems to be
    // a cancellation point
    case Workaround::Native:
      pass_task_ptr->stop(true);
      break;
    case Workaround::Input:
      if (enter_device.has_value()) {
        try {
          unsigned int retries;

          enter_device.value().send_enter_press();
          for (retries = 1;
               retries < max_enter_tries &&
               password &&
               pass_task_ptr->active() &&
               pass_task_ptr->wait(DEFAULT_ENTER_TIMEOUT) == std::future_status::timeout;
               retries++) {
            enter_device.value().send_enter_press();
          }

          if (retries >= max_enter_tries) {
            syslog(
                LOG_WARNING,
                "Failed to send enter input before the retries limit");
            conv_function(
                PAM_ERROR_MSG,
                S("Failed to send Enter press before the "
                  "retries limit, waiting for user to press it instead"));
          }
        } catch (std::runtime_error &err) {
          syslog(
              LOG_WARNING,
              "Failed to send enter input: %s",
              err.what());
          conv_function(
              PAM_ERROR_MSG,
              S("Failed to send Enter press, waiting for user "
                "to press it instead"));
        }
      } else {
        syslog(
            LOG_WARNING,
            "Insufficient permissions to create the fake device");
        conv_function(
            PAM_ERROR_MSG,
            S("Insufficient permissions to send Enter "
              "press, waiting for user to press it instead"));
      }
    // No automated workaround, the user has to press enter themselves
    case Workaround::Off:
      // We stop the thread (will block until the enter key is pressed if the
      // input wasn't focused or if the uinput device failed to send keypress)
      if (password) {
        pass_task_ptr->stop(false);
      }
    default:
      break;
  }

  // reset the terminal flags, so input does not stay hidden for example
  if (password) {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_flags);
  }
}

/**
 * The main function, runs the identification and authentication
 * @param  pamh     The handle to interface directly with PAM
 * @param  flags    Flags passed on to us by PAM, XORed
 * @param  argc     Amount of rules in the PAM config (disregarded)
 * @param  argv     Options defined in the PAM config
 * @param  ask_auth_tok True if we should ask for a password too
 * @return          Returns a PAM return code
 */
inline int identify(
  pam_handle_t *pamh,
  int flags,
  int argc,
  const char **argv,
  bool ask_auth_tok
) {
  INIReader config(CONFIG_FILE_PATH);
  openlog("pam_howdy", 0, LOG_AUTHPRIV);

  // Initialize gettext
  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  textdomain(GETTEXT_PACKAGE);

  // Error out if we could not read the config file
  if (config.ParseError() != 0) {
    syslog(LOG_ERR, "Failed to parse the configuration file: %d",
           config.ParseError());
    return PAM_SYSTEM_ERR;
  }

  // get all values from the config
  bool abort_if_ssh = config.GetBoolean("core", "abort_if_ssh", true);
  bool abort_if_lid_closed = config.GetBoolean("core", "abort_if_lid_closed", true);

  verbose = config.GetBoolean("core", "verbose", true);
  timeout_notice = config.GetBoolean("core", "timeout_notice", true);
  detection_notice = config.GetBoolean("core", "detection_notice", true);
  no_confirmation = config.GetBoolean("core", "no_confirmation", false);

  whitelist = split_string(config.GetString("core", "whitelist", ""), ',');
  blacklist = split_string(config.GetString("core", "blacklist", ""), ',');


  face_unlock = config.GetBoolean("face_authentication", "enable", true);
  max_face_auth_tries = (unsigned) config.GetInteger("face_authentication", "max_tries", 1);


  fingerprint = config.GetBoolean("fingerprint", "enable", false);
  fingerprint_timeout = config.GetInteger("fingerprint", "timeout", 30);
  max_fingerprint_tries = (unsigned) config.GetInteger("fingerprint", "max_tries", 3);


  password = config.GetBoolean("password", "enable", false);
  max_password_tries = (unsigned) config.GetInteger("password", "max_tries", 3);
  max_enter_tries = (unsigned) config.GetInteger64("password", "max_enter_tries", 5);
  bool password_nodelay = config.GetBoolean("password", "nodelay", false);
  Workaround workaround = get_workaround(config.GetString("password", "workaround", "off"));


  std::vector<const char*> password_args;


  // parse the options passed to the module
  std::string arg;
  for (int i = 0; i < argc; i++) {
    if (argv[i] == NULL) {
      continue;
    }

    arg = std::string(argv[i]);
    if (arg == "abort-if-ssh") {
      abort_if_ssh = true;
    } else if (arg.starts_with("abort-if-ssh=")) {
      abort_if_ssh = str_to_bool(arg.substr(13, arg.length()));
    }
    else if (arg == "abort-if-lid-closed") {
      abort_if_lid_closed = true;
    } else if (arg.starts_with("abort-if-lid-closed=")) {
      abort_if_lid_closed = str_to_bool(arg.substr(20, arg.length()));
    }
    else if (arg == "verbose") {
      verbose = true;
    } else if (arg.starts_with("verbose=")) {
      verbose = str_to_bool(arg.substr(8, arg.length()));
    }
    else if (arg == "timeout-notice") {
      timeout_notice = true;
    } else if (arg.starts_with("timeout-notice=")) {
      timeout_notice = str_to_bool(arg.substr(15, arg.length()));
    }
    else if (arg == "detection-notice") {
      detection_notice = true;
    } else if (arg.starts_with("detection-notice=")) {
      detection_notice = str_to_bool(arg.substr(17, arg.length()));
    }
    else if (arg == "no-confirmation") {
      no_confirmation = true;
    } else if (arg.starts_with("no-confirmation=")) {
      no_confirmation = str_to_bool(arg.substr(17, arg.length()));
    }
    else if (arg == "face-unlock") {
      face_unlock = true;
    } else if (arg.starts_with("face-unlock=")) {
      face_unlock = str_to_bool(arg.substr(12, arg.length()));
    }
    else if (arg == "fingerprint") {
      fingerprint = true;
    } else if (arg.starts_with("fingerprint=")) {
      fingerprint = str_to_bool(arg.substr(12, arg.length()));
    }
    else if (arg == "password") {
      password = true;
    } else if (arg.starts_with("password=")) {
      password = str_to_bool(arg.substr(9, arg.length()));
    }
    else if (arg =="password:nodelay") {
      password_nodelay = true;
    } else if (arg.starts_with("password:nodelay=")) {
      password_nodelay = str_to_bool(arg.substr(17, arg.length()));
    }
    else if (arg.starts_with("timeout=")) {
      face_auth_timeout = std::optional<const char*>(arg.substr(8, arg.length()).c_str());

      int fingerprint_timeout_opt = std::stol(arg.substr(8, arg.length()).c_str());
      fingerprint_timeout = fingerprint_timeout_opt < 1 ? UINT_MAX : (unsigned) fingerprint_timeout_opt;
    }
    else if (arg.starts_with("face-auth:timeout=")) {
      face_auth_timeout = std::optional<const char*>(arg.substr(18, arg.length()).c_str());
    }
    else if (arg.starts_with("fingerprint:timeout=")) {
      int fingerprint_timeout_opt = std::stol(arg.substr(20, arg.length()).c_str());
      fingerprint_timeout = fingerprint_timeout_opt < 1 ? UINT_MAX : (unsigned) fingerprint_timeout_opt;
    }
    else if (arg.starts_with("max-tries=")) {
      int max_face_auth_tries_opt = std::stoi(arg.substr(10, arg.length()).c_str());
      max_face_auth_tries = max_face_auth_tries_opt < 1 ? UINT_MAX : (unsigned) max_face_auth_tries_opt;

      int max_password_tries_opt = std::stoi(arg.substr(10, arg.length()).c_str());
      max_password_tries = max_password_tries_opt < 1 ? UINT_MAX : (unsigned) max_password_tries_opt;

      int max_fingerprint_tries_opt = std::stoi(arg.substr(10, arg.length()).c_str());
      max_fingerprint_tries = max_fingerprint_tries_opt < 1 ? UINT_MAX : (unsigned) max_fingerprint_tries_opt;
    }
    else if (arg.starts_with("face-auth:max-tries=")) {
      int max_face_auth_tries_opt = std::stoi(arg.substr(20, arg.length()).c_str());
      max_face_auth_tries = max_face_auth_tries_opt < 1 ? UINT_MAX : (unsigned) max_face_auth_tries_opt;
    }
    else if (arg.starts_with("password:max-tries=")) {
      int max_password_tries_opt = std::stoi(arg.substr(19, arg.length()).c_str());
      max_password_tries = max_password_tries_opt < 1 ? UINT_MAX : (unsigned) max_password_tries_opt;
    }
    else if (arg.starts_with("fingerprint:max-tries=")) {
      int max_fingerprint_tries_opt = std::stoi(arg.substr(22, arg.length()).c_str());
      max_fingerprint_tries = max_fingerprint_tries_opt < 1 ? UINT_MAX : (unsigned) max_fingerprint_tries_opt;
    }
    else if (arg.starts_with("password:max-enter-tries=")) {
      int max_enter_tries_opt = std::stoi(arg.substr(25, arg.length()).c_str());
      max_enter_tries = max_enter_tries_opt < 1 ? UINT_MAX : (unsigned) max_enter_tries_opt;
    }
    else if (arg.starts_with("password:workaround=")) {
      workaround = get_workaround(arg.substr(20, arg.length()));
    }
    else if (arg.starts_with("whitelist=")) {
      whitelist = split_string(arg.substr(10, arg.length()), ',');
    }
    else if (arg.starts_with("blacklist=")) {
      blacklist = split_string(arg.substr(10, arg.length()), ',');
    }
    else if (arg.starts_with("password:")) {
      password_args.push_back(arg.substr(9, arg.length()).c_str());
    }
  }

  if (password_nodelay) {
    password_args.push_back("nodelay");
  }
  password_args.shrink_to_fit();
  password = password && ask_auth_tok;

  //save the original terminal flags to they can be reset after password
  tcgetattr(STDIN_FILENO, &original_terminal_flags);

  // Get the username from PAM, needed to match correct face model
  pam_res = pam_get_user(pamh, const_cast<const char **>(&username), nullptr);
  if (pam_res != PAM_SUCCESS) {
    syslog(LOG_ERR, "Failed to get username");
    return pam_res;
  }

  if (config.GetBoolean("core", "disabled", false)) {
    syslog(LOG_INFO, "Skipped authentication, Howdy is disabled");
    return PAM_IGNORE;
  }

  if (abort_if_ssh) {
    if (checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") ||
        checkenv("SSH_TTY") || checkenv("SSHD_OPTS")) {
      syslog(LOG_INFO, "Skipped authentication, SSH session detected");
      return PAM_IGNORE;
    }
  }

  if (abort_if_lid_closed) {
    glob_t glob_result;

    // Get any files containing lid state
    int return_value = glob("/proc/acpi/button/lid/*/state", 0, nullptr, &glob_result);

    if (return_value != 0) {
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

  if ((!whitelist.empty() && !whitelist.count(std::string(username))) || blacklist.find(std::string(username)) != blacklist.end()) {
    return PAM_IGNORE;
  }

  // pre-check if this user has face model file
  auto model_path = std::string(USER_MODELS_DIR) + "/" + username + ".dat";
  struct stat stat_;
  if (stat(model_path.c_str(), &stat_) != 0) {
    // no face model found, disable face_auth
    face_unlock = false;
  }

  // Will contain PAM conversation structure
  struct pam_conv *conv = nullptr;
  const void **conv_ptr = const_cast<const void **>(reinterpret_cast<void **>(&conv));

  // Retrieve the PAM conversation structure
  pam_res = pam_get_item(pamh, PAM_CONV, conv_ptr);

  if (pam_res != PAM_SUCCESS || conv == nullptr || conv->conv == nullptr) {
    syslog(LOG_ERR, "Failed to acquire conversation");
    return PAM_SYSTEM_ERR;
  }

  // Wrap the PAM conversation function in our own, easier function
  conv_function = [conv](int msg_type, const char *msg_str) {
    const struct pam_message msg = {.msg_style = msg_type, .msg = msg_str};
    const struct pam_message *msgp = &msg;

    struct pam_response res = {};
    struct pam_response *resp = &res;
     
    return conv->conv(1, &msgp, &resp, conv->appdata_ptr);
  };

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);

  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  /*
   * ---------------------------------------------------------------
   * Dummy process to capture SIGINT to terminate everything
   * ---------------------------------------------------------------
   */
  optional_task<int> signal_task([&] {
    /*
     * Make the thread asynchronously cancellable.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    int sig;
    sigwait(&set, &sig);
    terminate = true;
    if (confirmation_type == ConfirmationType::Unset) {
      confirmation_type = ConfirmationType::Terminated;
    }
    convar.notify_all();
    return 0;
  });
  signal_task.activate();


  /*
   * ---------------------------------------------------------------
   * Howdy Python process
   * ---------------------------------------------------------------
   */
  pid_t child_pid;
  char *const args[] = {
      const_cast<char *const>(PYTHON_EXECUTABLE_PATH),
      const_cast<char *const>(COMPARE_PROCESS_PATH),
      username,
      const_cast<char *const>(face_auth_timeout.value_or(nullptr)),
      nullptr,
  };
  if (face_unlock) {
    if (posix_spawnp(
            &child_pid,
            PYTHON_EXECUTABLE_PATH,
            nullptr,
            nullptr,
            const_cast<char *const *>(args),
            nullptr) != 0) {
      syslog(
          LOG_ERR,
          "Can't spawn the howdy process: %s (%d)",
          strerror(errno),
          errno);
      return PAM_SYSTEM_ERR;
    }
  }

  /*
   * ---------------------------------------------------------------
   * Howdy task
   * ---------------------------------------------------------------
   *
   * Important:
   *
   * Howdy only becomes the winner when the Python process exits with
   * EXIT_SUCCESS.
   *
   * A timeout/error therefore does NOT prevent fingerprint/password
   * authentication from continuing.
   */
  optional_task<int> child_task([&] {
    /*
     * Make the thread asynchronously cancellable.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    int status = 0;
    bool retry = true;
    bool success = false;
    bool terminated = terminate;
    unsigned int retries = 0;
    while (retry) {
      waitpid(child_pid, &status, 0);
      success = (
          WIFEXITED(status) &&
          WEXITSTATUS(status) == EXIT_SUCCESS);
      terminated = (
          (status == CompareError::TERMINATED &&
           WTERMSIG(status) == CompareError::TERMINATED)
          || terminate);
      retry = (
        ++retries < max_face_auth_tries
        && !success
        && !terminated
        && confirmation_type == ConfirmationType::Unset);

      if (retry) {
        if (posix_spawnp(
                &child_pid,
                PYTHON_EXECUTABLE_PATH,
                nullptr,
                nullptr,
                const_cast<char *const *>(args),
                nullptr) != 0) {
          syslog(
              LOG_ERR,
              "Can't spawn the howdy process: %s (%d)",
              strerror(errno),
              errno);
          return PAM_SYSTEM_ERR;
        }
        howdy_error(status);
        if (detection_notice) {
          conv_function(PAM_TEXT_INFO, S("Please look directly into the camera"));
        }
      }
    }

    {
      std::unique_lock<std::mutex> lock(mutx);
      howdy_finished = true;
      howdy_success = success;
      howdy_status_code = status;

      if (confirmation_type == ConfirmationType::Unset) {
        if (success) {
          confirmation_type = ConfirmationType::Howdy;
        } else if (terminated || terminate) {
          confirmation_type = ConfirmationType::Terminated;
        } else {
          howdy_error(status);
        }
      }
    }
    convar.notify_all();

    return status;
  });
  child_task_ptr = &child_task;

  /*
   * ---------------------------------------------------------------
   * Password task
   * ---------------------------------------------------------------
   */
  struct PasswordPamContext {
    pam_handle_t *pamh = nullptr;
  };
  auto password_context =
      std::make_shared<PasswordPamContext>();

  optional_task<int> pass_task([&, password_context] {
    /*
     * Make the thread asynchronously cancellable.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    auto *handle = dlopen("/usr/lib/x86_64-linux-gnu/security/pam_unix.so", RTLD_NOW);
    if (!handle) {
      syslog(
          LOG_ERR,
          "Could not load pam_unix.so: %s",
          dlerror());

      {
        std::unique_lock<std::mutex> lock(mutx);

        password_finished = true;
        password_status_code = PAM_AUTHINFO_UNAVAIL;
      }

      convar.notify_all();

      return PAM_AUTHINFO_UNAVAIL;
    }
    dlerror();

    int (*password_pam_sm)(pam_handle_t *pamh, int flags, int argc, const char **argv);
    *(void**)(&password_pam_sm) = dlsym(handle, "pam_sm_authenticate");

    const char* err = dlerror();
    if (err) {
      syslog(
          LOG_ERR,
          "Could not load passwort authentication function: %s",
          err);

      {
        std::unique_lock<std::mutex> lock(mutx);

        password_finished = true;
        password_status_code = PAM_AUTHINFO_UNAVAIL;
      }

      convar.notify_all();

      return PAM_AUTHINFO_UNAVAIL;
    }

    char* orig_authtok;

    int rc = 0;
    bool retry = true;
    bool success = false;
    unsigned int retries = 0;
    while (retries < max_password_tries && retry) {
      orig_authtok = pamh->authtok;
      rc = password_pam_sm(pamh, flags, password_args.size(), password_args.data());

      success = (rc == PAM_SUCCESS);
      retry = (!success && !terminate && confirmation_type == ConfirmationType::Unset);

      retries++;
      if (retry) {
        pamh->authtok = orig_authtok;

        if (!password_nodelay) {
          sleep(2);
        }
        syslog(
            LOG_ERR,
            "Password authentication failed "
            "(attempt %d/%d)",
            retries,
            max_face_auth_tries);

        if (retries > 1 && verbose) {
          conv_function(PAM_TEXT_INFO, "");
        }
        if (retries < max_password_tries) {
          conv_function(
              PAM_ERROR_MSG,
              S("Password authentication failed, please try again"));
        } else {
          conv_function(
              PAM_ERROR_MSG,
              S("Password authentication failed"));
        }
      }
    }

    {
      std::unique_lock<std::mutex> lock(mutx);

      password_finished = true;
      password_success = success;
      password_status_code = rc;

      /*
       * Only PAM_SUCCESS may win the race.
       *
       * A password failure does NOT wake the main authentication
       * flow as a winner.
       */
      if (confirmation_type == ConfirmationType::Unset) {
        if (success) {
          confirmation_type = ConfirmationType::Pam;
        } else if (terminate) {
          confirmation_type = ConfirmationType::Terminated;
        }
      }
    }

    convar.notify_all();

    return rc;
  });
  pass_task_ptr = &pass_task;

  /*
   * ---------------------------------------------------------------
   * Fingerprint task
   * ---------------------------------------------------------------
   */
  auto fingerprint_authenticator =
    std::make_shared<FprintdAuthenticator>(
        username
    );
  optional_task<int> fingerprint_task([&] {
    /*
     * Make the thread asynchronously cancellable.
     *
     * authenticate() may block while fprintd waits for the
     * user's finger.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    FprintdAuthenticator::Result rc = fingerprint_authenticator->authenticate(
      conv_function,
      std::chrono::seconds(fingerprint_timeout),
      detection_notice,
      max_fingerprint_tries
    );

    {
      std::unique_lock<std::mutex> lock(mutx);

      bool terminated = (
        rc == FprintdAuthenticator::Result::Cancelled
        || rc == FprintdAuthenticator::Result::Disconnected
        || terminate
      );
      fingerprint_finished = true;
      fingerprint_success = (
        rc == FprintdAuthenticator::Result::Success
      );
      fingerprint_status_code = fingerprint_result_to_int(rc);

      /*
       * Only PAM_SUCCESS may win the race.
       *
       * A password failure does NOT wake the main authentication
       * flow as a winner.
       */
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

    convar.notify_all();

    return fingerprint_result_to_int(rc);
  });
  fingerprint_task_ptr = &fingerprint_task;

  /*
   * ---------------------------------------------------------------
   * EnterDevice
   * ---------------------------------------------------------------
   */
  if (
    workaround == Workaround::Input &&
    euidaccess("/dev/uinput", W_OK | R_OK) == 0
  ) {
    enter_device.emplace();
  }

  do_auth();

  fingerprint_authenticator->cancel();
  kill(child_pid, SIGKILL);

  signal_task.stop(true);

  switch (confirmation_type) {
    /*
     * ---------------------------------------------------------------
     * Fingerprint authenticated first
     * ---------------------------------------------------------------
     */
    case ConfirmationType::Fingerprint:
    {
      /*
      * Stop Howdy.
      */
      child_task.stop(true);

      /*
      * Fingerprint already returned PAM_SUCCESS.
      */
      fingerprint_task.stop(false);
      pam_res = fingerprint_task.get();

      /*
      * Print and log message
      */
      if (pam_res == PAM_SUCCESS) {
        if (verbose) {
          conv_function(PAM_TEXT_INFO, "");
        }
        conv_function(PAM_TEXT_INFO, S(std::format("Fingerprint authenticated as {}", username).c_str()));
        //conv_function(PAM_TEXT_INFO, ""); //TODO
        syslog(
            LOG_INFO,
            "Authenticated with fingerprint");
      }

      workaround_function(workaround);

      return pam_res;
    }

    /*
     * ---------------------------------------------------------------
     * Password authenticated first
     * ---------------------------------------------------------------
     */
    case ConfirmationType::Pam:
    {
      /*
      * Stop Howdy.
      */
      child_task.stop(true);

      /*
      * Stop fingerprint authentication.
      */
      fingerprint_task.stop(true);

      /*
      * Password task is the winner, therefore it can be joined
      * normally.
      */
      pass_task.stop(false);
      pam_res = pass_task.get();

      if (pam_res == PAM_SUCCESS) {
        syslog(
            LOG_INFO,
            "Authenticated with password");
        //conv_function(PAM_TEXT_INFO, ""); //TODO
        //conv_function(PAM_TEXT_INFO, ""); //TODO
      }
 
      return pam_res;
    }

    /*
     * ---------------------------------------------------------------
     * Howdy authenticated first
     * ---------------------------------------------------------------
     */
    case ConfirmationType::Howdy:
    {
      /*
      * Howdy has already finished successfully.
      */
      child_task.stop(false);
      pam_res = child_task.get();

      /*
      * Stop fingerprint.
      */
      fingerprint_task.stop(true);

      workaround_function(workaround);

      return howdy_status(pam_res);
    }

    /*
     * ---------------------------------------------------------------
     * Authentication was terminated
     * ---------------------------------------------------------------
     */
    case ConfirmationType::Terminated:
    {
      child_task.stop(true);

      /*
      * Stop fingerprint.
      */
      fingerprint_task.stop(true);

      workaround_function(Workaround::Native);

      raise(SIGINT);
      
      return PAM_CONV_ERR;
    }

    default:
      break;
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

  child_task.stop(false);

  fingerprint_task.stop(false);

  pass_task.stop(false);

  /*
   * Prefer the Howdy result for the existing Howdy error messages
   * when Howdy actually ran to completion.
   */
  if (howdy_finished) {
    pam_res = child_task.get();

    return howdy_status(pam_res);
  }

  /*
   * Otherwise return the fingerprint error.
   */
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
PAM_EXTERN int pam_sm_authenticate(
  pam_handle_t *pamh,
  int flags,
  int argc,
  const char **argv
) {
  return identify(pamh, flags, argc, argv, true);
}

// Called by PAM when a session is started, such as by the su command
PAM_EXTERN auto pam_sm_open_session(
  pam_handle_t *pamh,
  int flags,
  int argc,
  const char **argv
) -> int {
  return identify(pamh, flags, argc, argv, false);
}

// The functions below are required by PAM, but not needed in this module
PAM_EXTERN auto pam_sm_acct_mgmt(
  pam_handle_t *pamh,
  int flags,
  int argc,
  const char **argv
) -> int {
  return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_close_session(
  pam_handle_t *pamh,
  int flags,
  int argc,
  const char **argv
) -> int {
  return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc,
                                 const char **argv) -> int {
  return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                               const char **argv) -> int {
  return PAM_IGNORE;
}

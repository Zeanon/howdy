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

#include <INIReader.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include "enter_device.hh"
#include "main.hh"
#include "optional_task.hh"
#include <paths.hh>

const auto DEFAULT_TIMEOUT =
    std::chrono::duration<int, std::chrono::milliseconds::period>(500);
const auto MAX_ENTER_RETRIES = 5;

#define S(msg) gettext(msg)

/**
 * Inspect the status code returned by the compare process
 * @param  status        The status code
 * @param  conv_function The PAM conversation function
 */
inline auto howdy_error(int status,
                 const std::function<int(int, const char *)> &conv_function) {
  // If the process has exited
  if (WIFEXITED(status)) {
    // Get the status code returned
    status = WEXITSTATUS(status);

    switch (status) {
    case CompareError::NO_FACE_MODEL:
      syslog(LOG_NOTICE, "Failure: no face model known");
      break;
    case CompareError::TIMEOUT_REACHED:
      conv_function(PAM_TEXT_INFO, "");
      conv_function(PAM_ERROR_MSG, S("Face authentication timeout reached"));
      syslog(LOG_ERR, "Failure: face authentication timeout reached");
      break;
    case CompareError::ABORT:
      syslog(LOG_ERR, "Failure: general abort");
      break;
    case CompareError::TOO_DARK:
      conv_function(PAM_TEXT_INFO, "");
      conv_function(PAM_ERROR_MSG, S("Face detection image too dark"));
      syslog(LOG_ERR, "Failure: image too dark");
      break;
    case CompareError::INVALID_DEVICE:
      syslog(LOG_ERR,
             "Failure: not possible to open camera at configured path");
      break;
    default:
      conv_function(PAM_TEXT_INFO, "");
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
auto howdy_status(char *username, int status, const INIReader &config,
                  const std::function<int(int, const char *)> &conv_function)
    -> int {
  if (status != EXIT_SUCCESS) {
    return PAM_AUTH_ERR;
  }

  if (!config.GetBoolean("core", "no_confirmation", false)) {
    // Construct confirmation text from i18n string
    std::string confirm_text(S("Face authenticated as {}"));
    std::string identify_msg =
        confirm_text.replace(confirm_text.find("{}"), 2, std::string(username));
    conv_function(PAM_TEXT_INFO, identify_msg.c_str());
    conv_function(PAM_TEXT_INFO, "");
  }

  syslog(LOG_INFO, "Login approved");

  return PAM_SUCCESS;
}

inline auto howdy_init(const std::function<int(int, const char *)> &conv_function)
    -> int {
      return conv_function(PAM_TEXT_INFO, S("Please look directly into the camera"));
}

/**
 * Check if Howdy should be enabled according to the configuration and the
 * environment.
 * @param  config INI configuration
 * @param  username Username
 * @return        Returns PAM_AUTHINFO_UNAVAIL if it shouldn't be enabled,
 * PAM_SUCCESS otherwise
 */
auto check_enabled(const INIReader &config, const char *username) -> int {
  // Stop executing if Howdy has been disabled in the config
  if (config.GetBoolean("core", "disabled", false)) {
    syslog(LOG_INFO, "Skipped authentication, Howdy is disabled");
    return PAM_AUTHINFO_UNAVAIL;
  }

  // Stop if we're in a remote shell and configured to exit
  if (config.GetBoolean("core", "abort_if_ssh", true)) {
    if (checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") ||
        checkenv("SSH_TTY") || checkenv("SSHD_OPTS")) {
      syslog(LOG_INFO, "Skipped authentication, SSH session detected");
      return PAM_AUTHINFO_UNAVAIL;
    }
  }

  // Try to detect the laptop lid state and stop if it's closed
  if (config.GetBoolean("core", "abort_if_lid_closed", true)) {
    glob_t glob_result;

    // Get any files containing lid state
    int return_value =
        glob("/proc/acpi/button/lid/*/state", 0, nullptr, &glob_result);

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
          return PAM_AUTHINFO_UNAVAIL;
        }
      }
    }
    globfree(&glob_result);
  }

  // pre-check if this user has face model file
  auto model_path = std::string(USER_MODELS_DIR) + "/" + username + ".dat";
  struct stat stat_;
  if (stat(model_path.c_str(), &stat_) != 0) {
    return PAM_AUTHINFO_UNAVAIL;
  }

  return PAM_SUCCESS;
}

inline auto workaround_function(const Workaround &workaround,
                                optional_task<int> &pass_task,
                                const std::optional<EnterDevice> &enter_device,
                                const std::function<int(int, const char *)> &conv_function,
                                const termios &original_terminal_flags,
                                const bool ask_pass) {
  // We want to stop the password prompt, either by canceling the thread when
  // workaround is set to "native", or by emulating "Enter" input with
  // "input"
  switch (workaround) {
    // UNSAFE: We cancel the thread using pthread, pam_get_authtok seems to be
    // a cancellation point
    case Workaround::Native:
      if (pass_task.active()) {
        pass_task.stop(true);
      }
      break;
    case Workaround::Input:
      if (!enter_device.has_value()) {
        syslog(
            LOG_WARNING,
            "Insufficient permissions to create the fake device");
        conv_function(
            PAM_ERROR_MSG,
            S("Insufficient permissions to send Enter "
              "press, waiting for user to press it instead"));
      } else {
        try {
          int retries;

          enter_device.value().send_enter_press();

          for (retries = 0;
              retries < MAX_ENTER_RETRIES &&
              ask_pass &&
              pass_task.active() &&
              pass_task.wait(DEFAULT_TIMEOUT) == std::future_status::timeout;
              retries++) {

            enter_device.value().send_enter_press();
          }

          if (retries == MAX_ENTER_RETRIES) {
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
      }

    case Workaround::Manual:
      // We stop the thread (will block until the enter key is pressed if the
      // input wasn't focused or if the uinput device failed to send keypress)
      if (ask_pass &&
          pass_task.active()) {
        pass_task.stop(false);
      }
    default:
      break;
  }

  if (ask_pass) {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_flags);
  }
}

struct CustomConversationContext {
  const struct pam_conv *original;
  bool cancelled;
};

inline auto conv_wrapper(
  int num_msg,
  const struct pam_message **msg,
  struct pam_response **resp,
  void *appdata_ptr) -> int
{
  auto *ctx =
      static_cast<CustomConversationContext *>(appdata_ptr);

  int rc = ctx->original->conv(
      num_msg,
      msg,
      resp,
      ctx->original->appdata_ptr);

  if (rc == PAM_CONV_ERR) {
      ctx->cancelled = true;
  }
  return rc;
}

inline auto start_tasks(
  bool ask_pass,
  bool fingerprint,
  bool detection_notice,
  int fingerprint_timeout,
  bool &terminate,
  optional_task<int> &pass_task,
  optional_task<int> &child_task,
  optional_task<int> &fingerprint_task,
  optional_task<void> &fingerprint_timeout_task,
  bool &howdy_finished,
  bool &fingerprint_finished,
  bool &password_finished,
  ConfirmationType &confirmation_type,
  std::mutex &mutx,
  std::condition_variable &convar,
  const std::function<int(int, const char *)> &conv_function)
{
  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  child_task.activate();

  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (fingerprint) {
    fingerprint_task.activate();
    if (fingerprint_timeout > 0) {
      fingerprint_timeout_task.activate();
    }
  }
  if (ask_pass) {
    pass_task.activate();
  }

  usleep(100000);

  if (terminate || confirmation_type != ConfirmationType::Unset) {
    return;
  }
  if (ask_pass) {
    conv_function(PAM_TEXT_INFO, "");
  }
  if (detection_notice) {
    if (ask_pass) {
      conv_function(PAM_TEXT_INFO, "");
    }
    if ((conv_function(PAM_TEXT_INFO, S("Facial authentication..."))) != PAM_SUCCESS ||
        howdy_init(conv_function) != PAM_SUCCESS) {
      syslog(LOG_ERR, "Failed to send detection notice");
    }
  }
  if (fingerprint) {
    conv_function(PAM_TEXT_INFO, "");
    conv_function(PAM_TEXT_INFO, S("Fingerprint authentication..."));
  }

  /*
  * ---------------------------------------------------------------
  * Wait for first successful authentication
  * ---------------------------------------------------------------
  */
  if (!terminate && confirmation_type == ConfirmationType::Unset) {
    std::unique_lock<std::mutex> lock(mutx);

    convar.wait(lock, [&] {
      /*
      * A successful authentication has won.
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
            (!ask_pass || password_finished);
    });
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
auto identify(pam_handle_t *pamh, int flags, int argc, const char **argv,
              bool ask_auth_tok) -> int {
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

  std::string workaround_string = config.GetString("core", "workaround", "off");
  std::optional<Workaround> fingerprint_workaround_opt = std::nullopt;

  int max_tries = config.GetInteger("core", "max_tries", 1);
  int max_password_tries = config.GetInteger64("core", "max_password_tries", 3);
  auto fingerprint = config.GetBoolean("core", "use_fingerprint", false);
  auto detection_notice = config.GetBoolean("core", "detection_notice", true);
  auto fingerprint_timeout = config.GetInteger("core", "fingerprint_timeout", -1);
  auto service_name = config.GetString("core", "service_name", "howdy-fingerprint");
  auto password_service_name = config.GetString("core", "password_service_name", "howdy-password");
  std::optional<const char*> timeout = std::nullopt;

  for (int i = 0; i < argc; i++) {
    auto arg = std::string(argv[i]);
    if (arg == "fingerprint") {
      fingerprint = true;
    } else if (arg.starts_with("fingerprint=")) {
      auto temp = arg.substr(12, arg.length());
      fingerprint = temp == "true" || temp == "1";
    } else if (arg == "detection-notice") {
      detection_notice = true;
    } else if (arg.starts_with("detection-notice=")) {
      auto temp = arg.substr(17, arg.length());
      detection_notice = temp == "true" || temp == "1";
    } else if (arg.starts_with("fingerprint-timeout=")) {
      fingerprint_timeout = std::stoi(arg.substr(20, arg.length()).c_str());
    } else if (arg.starts_with("max-tries=")) {
      max_tries = std::stoi(arg.substr(10, arg.length()).c_str());
    } else if (arg.starts_with("max-password-tries=")) {
      max_password_tries = std::stoi(arg.substr(19, arg.length()).c_str());
    } else if (arg.starts_with("workaround=")) {
      workaround_string = arg.substr(11, arg.length());
    } else if (arg.starts_with("fingerprint-workaround=")) {
      fingerprint_workaround_opt = std::optional<Workaround>(get_workaround(arg.substr(23, arg.length())));
    } else if (arg.starts_with("service-name=")) {
      service_name = arg.substr(13, arg.length());
    } else if (arg.starts_with("password-service-name=")) {
      password_service_name = arg.substr(22, arg.length());
    } else if (arg.starts_with("timeout=")) {
      timeout = std::optional<const char*>(arg.substr(8, arg.length()).c_str());
    }
  }

  if (max_tries < 1) {
    syslog(LOG_ERR, "Max-tries has to be greater than 0");
    return PAM_SYSTEM_ERR;
  }

  if (max_password_tries < 1) {
    syslog(LOG_ERR, "Max-password-tries has to be greater than 0");
    return PAM_SYSTEM_ERR;
  }

  Workaround workaround = get_workaround(workaround_string);
  Workaround fingerprint_workaround = fingerprint_workaround_opt.value_or(
                                        get_workaround(
                                          config.GetString("core", "fingerprint-workaround", workaround_string)
                                        )
                                      );
  
  bool ask_pass = ask_auth_tok && (workaround != Workaround::Off || (fingerprint_workaround != Workaround::Off && fingerprint));

  termios original_terminal_flags;
  tcgetattr(STDIN_FILENO, &original_terminal_flags);

  bool terminate = false;
  // Will contain the responses from PAM functions
  int pam_res = PAM_IGNORE;

  // Get the username from PAM, needed to match correct face model
  char *username = nullptr;
  pam_res = pam_get_user(pamh, const_cast<const char **>(&username), nullptr);
  if (pam_res != PAM_SUCCESS) {
    syslog(LOG_ERR, "Failed to get username");
    return pam_res;
  }

  // Check whether Howdy should run
  pam_res = check_enabled(config, username);
  if (pam_res != PAM_SUCCESS) {
    return pam_res;
  }

  // Will contain PAM conversation structure
  struct pam_conv *conv = nullptr;
  const void **conv_ptr =
      const_cast<const void **>(reinterpret_cast<void **>(&conv));

  // Retrieve the PAM conversation structure
  pam_res = pam_get_item(pamh, PAM_CONV, conv_ptr);

  if (pam_res != PAM_SUCCESS || conv == nullptr || conv->conv == nullptr) {
    syslog(LOG_ERR, "Failed to acquire conversation");
    return PAM_SYSTEM_ERR;
  }

  // Wrap the PAM conversation function in our own, easier function
  auto conv_function = [conv](int msg_type, const char *msg_str) {
    const struct pam_message msg = {.msg_style = msg_type, .msg = msg_str};
    const struct pam_message *msgp = &msg;

    struct pam_response res = {};
    struct pam_response *resp = &res;
     
    return conv->conv(1, &msgp, &resp, conv->appdata_ptr);
  };

  /*
   * ---------------------------------------------------------------
   * Authentication state
   * ---------------------------------------------------------------
   */
  std::mutex mutx;
  std::condition_variable convar;

  ConfirmationType confirmation_type =
      ConfirmationType::Unset;

  bool howdy_finished = false;
  bool howdy_success = false;

  bool fingerprint_finished = false;
  bool fingerprint_success = false;

  bool password_finished = false;
  bool password_success = false;

  int howdy_status_code = PAM_AUTH_ERR;
  int fingerprint_status_code = PAM_AUTH_ERR;
  int password_status_code = PAM_AUTH_ERR;

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

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    //pthread_sigmask(SIG_BLOCK, &set, nullptr);

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
      const_cast<char *const>(timeout.value_or(nullptr)),
      nullptr,
  };
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
    int retries = 0;
    bool success = false;
    bool terminated = terminate;
    while (retries < max_tries
         && !success
         && !terminated) {
      waitpid(child_pid, &status, 0);
      success = (
          WIFEXITED(status) &&
          WEXITSTATUS(status) == EXIT_SUCCESS);
      terminated = (
          (status == 15 &&
           WTERMSIG(status) == 15)
          || terminate);
      
      if (++retries < max_tries && !success && !terminated) {
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
        howdy_error(status, conv_function);
        if (detection_notice) {
          howdy_init(conv_function);
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
        } else if (terminated) {
          confirmation_type = ConfirmationType::Terminated;
        } else {
          howdy_error(status, conv_function);
        }
      }
    }
    convar.notify_all();

    return status;
  });

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

    int rc = 0;
    int retries = 0;
    bool success = false;
    bool terminated = terminate;
    while (retries < max_password_tries
           && !success
           && !terminated) {
      /*
      * The cleanup handler makes sure pam_end() is executed when
      * this thread is cancelled.
      */
      auto cleanup = [](void *arg) {
        auto *ctx =
            static_cast<PasswordPamContext *>(arg);
        if (ctx->pamh != nullptr) {
          pam_end(ctx->pamh, PAM_ABORT);
          ctx->pamh = nullptr;
        }
      };

      /*
      * The PAM conversation structure itself can be copied.
      *
      * The PAM handle MUST remain separate.
      */
      CustomConversationContext password_conv_ctx{
          .original = conv,
          .cancelled = false,
      };
      struct pam_conv password_conv{
          .conv = conv_wrapper,
          .appdata_ptr = &password_conv_ctx,
      };

      pam_handle_t *password_pamh = nullptr;

      rc = pam_start(
          password_service_name.c_str(),
          username,
          &password_conv,
          &password_pamh);

      if (rc != PAM_SUCCESS) {
        syslog(
            LOG_ERR,
            "Password pam_start() failed: %d",
            rc);

        {
          std::unique_lock<std::mutex> lock(mutx);

          password_finished = true;
          password_status_code = rc;
        }

        convar.notify_all();

        return rc;
      }

      password_context->pamh = password_pamh;

      /*
      * Register cleanup BEFORE entering pam_authenticate().
      */
      pthread_cleanup_push(cleanup, password_context.get());

      rc = pam_authenticate(
          password_pamh,
          0);

      /*
      * Normal completion:
      * unregister cleanup without executing it.
      */
      pthread_cleanup_pop(0);

      /*
      * pam_authenticate() has returned, therefore it is safe to
      * terminate this independent PAM transaction.
      */
      password_context->pamh = nullptr;

      int end_rc = pam_end(
          password_pamh,
          rc);

      if (rc == PAM_SUCCESS && end_rc != PAM_SUCCESS) {
        rc = end_rc;
      }

      success = (rc == PAM_SUCCESS);
      terminated = (password_conv_ctx.cancelled || terminate);

      retries++;
      if (!success && !terminated) {
        syslog(
            LOG_NOTICE,
            "Password authentication failed "
            "(attempt %d/%d)",
            retries,
            max_tries);

        if (retries != 1) {
          conv_function(PAM_TEXT_INFO, "");
        }
        if (retries < max_tries) {
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
        } else if (terminated) {
          confirmation_type = ConfirmationType::Terminated;
        }
      }
    }

    convar.notify_all();

    return rc;
  });

  /*
   * ---------------------------------------------------------------
   * Fingerprint PAM task
   * ---------------------------------------------------------------
   *
   * This is a completely independent PAM transaction.
   *
   * NEVER reuse `pamh` here.
   *
   * The service name refers to:
   *
   *     /etc/pam.d/howdy-fingerprint
   *
   * which should contain:
   *
   *     auth required pam_fprintd.so
   */
  struct FingerprintPamContext {
    pam_handle_t *pamh = nullptr;
  };
  auto fingerprint_context =
      std::make_shared<FingerprintPamContext>();

  optional_task<int> fingerprint_task([&, fingerprint_context] {
    /*
     * Make the thread asynchronously cancellable.
     *
     * pam_authenticate() may block while fprintd waits for the
     * user's finger.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    /*
     * The cleanup handler makes sure pam_end() is executed when
     * this thread is cancelled.
     */
    auto cleanup = [](void *arg) {
      auto *ctx =
          static_cast<FingerprintPamContext *>(arg);
      if (ctx->pamh != nullptr) {
        pam_end(ctx->pamh, PAM_ABORT);
        ctx->pamh = nullptr;
      }
    };

    pam_handle_t *finger_pamh = nullptr;

    /*
     * The PAM conversation structure itself can be copied.
     *
     * The PAM handle MUST remain separate.
     */
      CustomConversationContext fingerprint_conv_ctx{
          .original = conv,
          .cancelled = false,
      };

      struct pam_conv fingerprint_conv{
          .conv = conv_wrapper,
          .appdata_ptr = &fingerprint_conv_ctx,
      };

    int rc = pam_start(
        service_name.c_str(),
        username,
        &fingerprint_conv,
        &finger_pamh);

    if (rc != PAM_SUCCESS) {
      syslog(
          LOG_ERR,
          "Fingerprint pam_start() failed: %d",
          rc);
      {
        std::unique_lock<std::mutex> lock(mutx);

        fingerprint_finished = true;
        fingerprint_status_code = rc;
      }

      convar.notify_all();

      return rc;
    }

    fingerprint_context->pamh = finger_pamh;

    /*
     * Register cleanup BEFORE entering pam_authenticate().
     */
    pthread_cleanup_push(cleanup, fingerprint_context.get());

    rc = pam_authenticate(
        finger_pamh,
        0);

    /*
     * Normal completion:
     * unregister cleanup without executing it.
     */
    pthread_cleanup_pop(0);

    /*
     * pam_authenticate() has returned, therefore it is safe to
     * terminate this independent PAM transaction.
     */
    fingerprint_context->pamh = nullptr;

    int end_rc = pam_end(
        finger_pamh,
        rc);

    if (rc == PAM_SUCCESS && end_rc != PAM_SUCCESS) {
      rc = end_rc;
    }

    bool success = (rc == PAM_SUCCESS);
    bool terminated = (fingerprint_conv_ctx.cancelled || terminate);

    {
      std::unique_lock<std::mutex> lock(mutx);

      fingerprint_finished = true;
      fingerprint_success = success;
      fingerprint_status_code = rc;

      /*
       * Only PAM_SUCCESS may win the race.
       *
       * A fingerprint failure does NOT wake the main authentication
       * flow as a winner.
       */
      if (confirmation_type == ConfirmationType::Unset) {
        if (success) {
          confirmation_type = ConfirmationType::Fingerprint;
        } else if (terminated) {
          confirmation_type = ConfirmationType::Terminated;
        }
      }
    }

    convar.notify_all();

    return rc;
  });

  /*
   * ---------------------------------------------------------------
   * Fingerprint timeout task
   * ---------------------------------------------------------------
   *
   * Terminates the fingerprint task if a timeout is exceeded.
   */
  optional_task<void> fingerprint_timeout_task([&] {
    if (fingerprint_task.wait(std::chrono::seconds(fingerprint_timeout)) ==
        std::future_status::timeout) {

      {
        std::unique_lock<std::mutex> lock(mutx);

        if (!fingerprint_finished &&
            confirmation_type == ConfirmationType::Unset) {

          conv_function(PAM_ERROR_MSG, "Fingerprint authentication timeout reached");
          syslog(LOG_ERR, "Failure: fingerprint authentication timeout reached");

          fingerprint_status_code = PAM_AUTH_ERR;
          fingerprint_finished = true;
        }
      }

      /*
      * This cancels the thread which is currently blocked in
      * pam_authenticate().
      *
      * fingerprint_pam_cleanup() then calls pam_end(..., PAM_ABORT).
      */
      fingerprint_task.stop(true);

      convar.notify_all();
    }
  });

  /*
   * ---------------------------------------------------------------
   * EnterDevice
   * ---------------------------------------------------------------
   */
  std::optional<EnterDevice> enter_device =
          (workaround == Workaround::Input ||
           fingerprint_workaround == Workaround::Input) &&
          euidaccess("/dev/uinput", W_OK | R_OK) == 0
          ? std::optional<EnterDevice>{std::in_place}
          : std::nullopt;


  start_tasks(
    ask_pass,
    fingerprint,
    detection_notice,
    fingerprint_timeout,
    terminate,
    pass_task,
    child_task,
    fingerprint_task,
    fingerprint_timeout_task,
    howdy_finished,
    fingerprint_finished,
    password_finished,
    confirmation_type,
    mutx,
    convar,
    conv_function
  );

  if (signal_task.active()) {
    signal_task.stop(true);
  }

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
      kill(child_pid, SIGTERM);
      child_task.stop(true);

      /*
      * Fingerprint already returned PAM_SUCCESS.
      */
      fingerprint_task.stop(false);
      pam_res = fingerprint_task.get();
      if (fingerprint_timeout_task.active()) {
        fingerprint_timeout_task.stop(false);
      }

      /*
      * Print and log message
      */
      std::string confirm_text(S("Fingerprint authenticated as {}"));
      conv_function(PAM_TEXT_INFO, confirm_text.replace(confirm_text.find("{}"), 2, std::string(username)).c_str());
      conv_function(PAM_TEXT_INFO, "");
      syslog(
          LOG_INFO,
          "Authenticated with fingerprint");

      workaround_function(fingerprint_workaround, pass_task, enter_device, conv_function, original_terminal_flags, ask_pass);

      return pam_res;
    }

    /*
    * ---------------------------------------------------------------
    * Password authenticated first
    * ---------------------------------------------------------------
    */
    case ConfirmationType::Pam:
    {
      syslog(
          LOG_INFO,
          "Authenticated with password");
      conv_function(PAM_TEXT_INFO, "");

      /*
      * Stop Howdy.
      */
      kill(child_pid, SIGTERM);
      child_task.stop(true);

      /*
      * Stop fingerprint authentication.
      */
      if (fingerprint_task.active()) {
        fingerprint_task.stop(true);
      }
      if (fingerprint_timeout_task.active()) {
        fingerprint_timeout_task.stop(false);
      }

      /*
      * Password task is the winner, therefore it can be joined
      * normally.
      */
      pass_task.stop(false);
      pam_res = pass_task.get();
 
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
      if (fingerprint_task.active()) {
        fingerprint_task.stop(true);
      }
      if (fingerprint_timeout_task.active()) {
        fingerprint_timeout_task.stop(false);
      }

      workaround_function(workaround, pass_task, enter_device, conv_function, original_terminal_flags, ask_pass);

      return howdy_status(
          username,
          pam_res,
          config,
          conv_function);
    }

    /*
    * ---------------------------------------------------------------
    * Authentication was terminated
    * ---------------------------------------------------------------
    */
    case ConfirmationType::Terminated:
    {
      kill(child_pid, SIGTERM);
      child_task.stop(false);

      /*
      * Stop fingerprint.
      */
      if (fingerprint_task.active()) {
        fingerprint_task.stop(true);
      }
      if (fingerprint_timeout_task.active()) {
        fingerprint_timeout_task.stop(false);
      }

      workaround_function(Workaround::Native, pass_task, enter_device, conv_function, original_terminal_flags, ask_pass);

      raise(SIGINT);
      
      return PAM_AUTH_ERR;
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

  if (child_task.active()) {
    child_task.stop(false);
  }

  if (fingerprint && fingerprint_task.active()) {
    fingerprint_task.stop(false);
    if (fingerprint_timeout_task.active()) {
      fingerprint_timeout_task.stop(false);
    }
  }

  if (ask_pass &&
      pass_task.active()) {
    pass_task.stop(false);
  }

  /*
   * Prefer the Howdy result for the existing Howdy error messages
   * when Howdy actually ran to completion.
   */
  if (howdy_finished) {
    pam_res = child_task.get();

    return howdy_status(
        username,
        pam_res,
        config,
        conv_function);
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
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                    const char **argv) -> int {
  return identify(pamh, flags, argc, argv, true);
}

// Called by PAM when a session is started, such as by the su command
PAM_EXTERN auto pam_sm_open_session(pam_handle_t *pamh, int flags, int argc,
                                    const char **argv) -> int {
  return identify(pamh, flags, argc, argv, false);
}

// The functions below are required by PAM, but not needed in this module
PAM_EXTERN auto pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc,
                                 const char **argv) -> int {
  return PAM_IGNORE;
}
PAM_EXTERN auto pam_sm_close_session(pam_handle_t *pamh, int flags, int argc,
                                     const char **argv) -> int {
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

#ifndef MAIN_H_
#define MAIN_H_

#include <cstring>
#include <string>
#include <unistd.h>
#include <cstdint>

#include "fingerprint_authenticator.hh"

enum class ConfirmationType {
  Unset,
  Terminated,
  Howdy,
  Fingerprint,
  Pam
};
enum class Workaround : std::uint8_t {
  Unset,
  Manual,
  Input,
  Native
};

// Exit status codes returned by the compare process
enum CompareError : std::uint8_t {
  NO_FACE_MODEL = 10,
  TIMEOUT_REACHED = 11,
  ABORT = 12,
  TOO_DARK = 13,
  INVALID_DEVICE = 14,
  TERMINATED = 15,
  RUBBERSTAMP = 20,
};

inline auto get_workaround(const std::string &workaround) -> Workaround {
  if (workaround == "manual") {
    return Workaround::Manual;
  }
  
  if (workaround == "input") {
    return Workaround::Input;
  }

  if (workaround == "native") {
    return Workaround::Native;
  }

  return Workaround::Unset;
}

inline std::set<std::string> split_string(const std::string& str, char delimiter) {
    std::set<std::string> tokens;
    if (str == "") {
      return tokens;
    }
    size_t start = 0;
    size_t end = str.find(delimiter);
    
    while (end != std::string::npos) {
        tokens.insert(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    
    tokens.insert(str.substr(start));
    return tokens;
}

inline bool str_to_bool(const std::string& str) {
  return str == "true" || str == "on" || str == "1";
}

inline int fingerprint_result_to_int(FprintdAuthenticator::Result result) {
  switch (result) {
    case FprintdAuthenticator::Result::Success:
      return PAM_SUCCESS;
    case FprintdAuthenticator::Result::NoDevice:
    case FprintdAuthenticator::Result::Busy:
    case FprintdAuthenticator::Result::NoEnrolledPrints:
      return PAM_AUTHINFO_UNAVAIL;
    case FprintdAuthenticator::Result::NoMatch:
      return PAM_AUTH_ERR;
    case FprintdAuthenticator::Result::MaxTries:
      return PAM_MAXTRIES;
    case FprintdAuthenticator::Result::Timeout:
    case FprintdAuthenticator::Result::Cancelled:
    case FprintdAuthenticator::Result::Disconnected:
    case FprintdAuthenticator::Result::Error:
    default:
      return PAM_CONV_ERR;
  }
}

/**
 * Check if an environment variable exists either in the environ array or using
 * getenv.
 * @param name The name of the environment variable.
 * @return The value of the environment variable or nullptr if it doesn't exist
 * or environ is nullptr.
 * @note This function was created because `getenv` wasn't working properly in
 * some contexts (like sudo).
 */
auto checkenv(const char *name) -> bool {
  if (std::getenv(name) != nullptr) {
    return true;
  }

  auto len = strlen(name);

  for (char **env = environ; *env != nullptr; env++) {
    if (strncmp(*env, name, len) == 0) {
      return true;
    }
  }

  return false;
}

#endif // MAIN_H_

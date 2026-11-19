#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "config.hh"
#include "face_backend.hh"
#include "video_capture.hh"

namespace howdy {

// Rubber stamps are extra checks that run after a successful face recognition
// and can still deny the authentication, see [rubberstamps] in config.ini.

// Thrown inside a stamp when the authentication was cancelled
class StampCancelled : public std::exception {
  public:
	[[nodiscard]] const char* what() const noexcept override {
		return "Rubber stamp cancelled";
	}
};

enum class StampResult {
	// All stamps passed
	Passed,
	// A stamp denied the authentication
	Denied,
	// The authentication was cancelled while the stamps ran
	Cancelled,
};

// Everything the stamps need from the authenticator
struct StampContext {
	const Config& config;
	VideoCapture& video_capture;
	FaceBackend& backend;
	cv::Ptr<cv::CLAHE> clahe;
	// Scaling of the frames, as used for the authentication
	double scaling_factor;
	// [video] rotate, only the fixed modes are applied
	int rotate;
	const std::atomic<bool>& cancel;
	// Update the main text and the subtext of the auth popup
	std::function<void(const std::string&)> set_message;
	std::function<void(const std::string&)> set_subtext;
};

// A line of stamp_rules:  stamptype  timeout  (failsafe | faildeadly)  [key=value ...]
struct StampRule {
	std::string type;
	double timeout;
	// A stamp that runs into the timeout denies the authentication with failsafe
	// and lets it pass with faildeadly
	bool failsafe;
	// The optional arguments at the end of the line
	std::string arguments;
};

// std::nullopt if the line is not a valid rule
std::optional<StampRule> parse_stamp_rule(const std::string& line);

// Run the stamps from the stamp_rules of the config in order. Stops at the first
// stamp that denies the authentication.
StampResult run_rubberstamps(const StampContext& context);

} // namespace howdy

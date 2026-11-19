#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "face_backend.hh"
#include "video_capture.hh"

namespace howdy {

// The user cancelled the enrollment
class EnrollCancelled : public std::runtime_error {
  public:
	EnrollCancelled() : std::runtime_error("Enrollment cancelled") {
	}
};

// How long the steps of the capture take, in seconds
struct EnrollTiming {
	// Time to read the first prompt
	double init_wait = 2;
	// Time to move after each prompt, the frames of it are discarded
	double settle = 2.8;
	// Time per step to find frames that can be used
	double step = 6.0;
	// Time to look at the result
	double end_wait = 1;
};

struct EnrollSettings {
	FaceBackend& backend;
	VideoCapture& capture;
	// Frames with more than this percentage of dark pixels are skipped
	double dark_threshold;
	// Terminal output, called from the thread that runs the dialog
	std::function<void(const std::string&)> say;
	EnrollTiming timing{};
	// The window the dialog belongs to: it stays in front of it. None if null.
	GtkWindow* parent = nullptr;
	// [video] rotate: only the fixed modes 3 and 4 are applied, so the model is
	// built from the same upright image that is used for authentication
	int rotate = 0;
};

// Guided multi-sample face capture used by `howdy add`.
//
// A model used to be one descriptor from the first frame that showed a face:
// one pose, one expression, one IR exposure. Authentication is a nearest
// neighbour distance to a stored descriptor, so that single snapshot often
// landed just past the certainty threshold when the head or the light moved a
// little. This guides the user through small head movements in a dialog with
// the camera image and keeps a few more descriptors per pose.
//
// GTK has to be initialized. Blocks until the capture is done. Throws
// EnrollCancelled if the user cancels and std::runtime_error with a message
// for the user if no samples can be captured.
std::vector<Encoding> capture_guided_samples(const EnrollSettings& settings);

} // namespace howdy

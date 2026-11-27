#include "rubberstamps/nod.hh"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <vector>

#include "rotation.hh"
#include "util.hh"

namespace howdy {

namespace {
// How many frames in a row show no face before another way to turn them is tried
constexpr int EMPTY_FRAMES_BEFORE_TURN = 5;
} // namespace

NodStamp::NodStamp() {
	options["min_distance"] = 6L;
	options["min_directions"] = 2L;
}

bool NodStamp::run(const StampContext& context, const StampRule& rule) {
	context.set_message(S("Nod to confirm"));
	context.set_subtext(S("Shake your head to abort"));

	const double min_distance = static_cast<double>(get_int("min_distance"));
	const auto min_directions = static_cast<std::size_t>(get_int("min_directions"));
	// With the alternating modes the one that shows a face is kept, the movement of the nose can only be
	// followed in frames that are turned the same way
	const auto rotations = upright_rotations(context.rotate);
	std::size_t rotation = 0;
	// Frames in a row without a face. One alone says nothing about the way the frames are turned: an
	// IR emitter may light only every second one
	int empty_frames = 0;

	// Relative distance between the 2 eyes in the last frame, used to calculate
	// the distance the nose traveled in relation to the size of the face
	double last_reldist = -1;
	// Last point the nose was at, for x and y
	double last_nosepoint[2] = {-1, -1};
	// Records the successful nods and their direction
	std::vector<bool> recorded_nods[2];

	const auto start = std::chrono::steady_clock::now();
	while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < rule.timeout) {
		check_cancelled(context);

		// Read a frame from the camera
		cv::Mat frame;
		cv::Mat gsframe;
		try {
			std::tie(frame, gsframe) = context.video_capture.read_frame();
		} catch (const NoDeviceException& exc) {
			log(LOG_ERR, exc.what());
			return false;
		}

		rotate_frame(frame, rotations[rotation], 0);
		rotate_frame(gsframe, rotations[rotation], 0);
		if (context.scaling_factor != 1) {
			cv::resize(frame, frame, cv::Size(), context.scaling_factor, context.scaling_factor, cv::INTER_AREA);
			cv::resize(gsframe, gsframe, cv::Size(), context.scaling_factor, context.scaling_factor, cv::INTER_AREA);
		}

		// Apply CLAHE to get a better picture
		context.clahe->apply(gsframe, gsframe);

		// Only continue if exactly 1 face is visible in the frame
		const auto faces = context.backend.detect(gsframe);
		if (!faces.empty()) {
			empty_frames = 0;
		} else if (rotations.size() > 1 && ++empty_frames == EMPTY_FRAMES_BEFORE_TURN) {
			// Try the next way, positions of the one before do not count there
			rotation = (rotation + 1) % rotations.size();
			empty_frames = 0;
			last_nosepoint[0] = last_nosepoint[1] = -1;
			last_reldist = -1;
		}
		if (faces.size() != 1) {
			continue;
		}

		// The position of the eyes and the tip of the nose
		const auto landmarks = context.backend.motion_landmarks(frame, faces.front());
		if (landmarks.size() < 5) {
			continue;
		}

		// Relative distance between the 2 eyes, from the outer corner of one to the outer corner of the
		// other, averaged with the last frame to smooth it out. The first four points are the corners of
		// the eyes, dlib and OpenVINO number them in another order.
		const auto [left, right] = std::minmax({landmarks[0].x, landmarks[1].x, landmarks[2].x, landmarks[3].x});
		const auto reldist = static_cast<double>(right - left);
		const double avg_reldist = (last_reldist + reldist) / 2;

		// Horizontal movement (shaking the head) and vertical movement (nodding)
		for (int axis = 0; axis < 2; ++axis) {
			const auto nosepoint = static_cast<double>(axis == 0 ? landmarks[4].x : landmarks[4].y);

			// If this is the first frame set the previous values to the current ones
			if (last_nosepoint[axis] == -1) {
				last_nosepoint[axis] = nosepoint;
			}

			// The relative movement is the distance traveled divided by the eye distance
			const double movement = (nosepoint - last_nosepoint[axis]) * 100 / std::max(avg_reldist, 1.0);

			// If the movement is over the minimal distance threshold
			auto& nods = recorded_nods[axis];
			if (movement < -min_distance || movement > min_distance) {
				// Only count a nod if it is the first one or in the other direction than the last one
				if (nods.empty() || nods.back() != (movement < 0)) {
					nods.push_back(movement < 0);
				}
			}

			// Check if we have nodded enough on this axis
			if (nods.size() >= min_directions) {
				const bool confirmed = axis == 1;
				context.set_message(confirmed ? S("Confirmed authentication") : S("Aborted authentication"));
				context.set_subtext("");

				// Nodding yes confirms, shaking no aborts
				cancelable_sleep(context, std::chrono::milliseconds(800));
				return confirmed;
			}

			// Save the relative distance and the nosepoint for the next frame
			last_reldist = reldist;
			last_nosepoint[axis] = nosepoint;
		}
	}

	// The timeout was hit
	return !rule.failsafe;
}

} // namespace howdy

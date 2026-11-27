#pragma once

#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy {

// Values of [video] rotate
//  0  landscape only
//  1  landscape and portrait (alternating per frame)
//  2  portrait only (direction alternating per frame)
//  3  always 90 degrees counterclockwise, for sideways mounted cameras
//  4  always 90 degrees clockwise, for sideways mounted cameras
inline constexpr int ROTATE_FIXED_CCW = 3;
inline constexpr int ROTATE_FIXED_CW = 4;

// Modes 2 - 4 deliver portrait frames, so width and height are swapped
[[nodiscard]] inline bool rotation_is_portrait(int rotate) noexcept {
	return rotate >= 2;
}

// Only the fixed modes give a stable orientation. The add and test commands
// use them alone, the alternating modes would make the image flicker and
// store models of different orientations.
[[nodiscard]] inline int fixed_rotation(int rotate) noexcept {
	return (rotate == ROTATE_FIXED_CCW || rotate == ROTATE_FIXED_CW) ? rotate : 0;
}

// The ways a frame may have to be turned for an upright face, as fixed modes (0 is not turned):
// one for the fixed modes, all of them for the alternating ones
[[nodiscard]] inline std::vector<int> upright_rotations(int rotate) {
	switch (rotate) {
		case 1: return {0, ROTATE_FIXED_CCW, ROTATE_FIXED_CW};
		case 2: return {ROTATE_FIXED_CCW, ROTATE_FIXED_CW};
		default: return {fixed_rotation(rotate)};
	}
}

// Rotate the frame in place according to the rotate mode. Mode 2 alternates
// the direction on every frame, which lines up badly with IR emitters that
// light every second frame, so 3 and 4 are fixed. Unknown modes leave the
// frame untouched.
inline void rotate_frame(cv::Mat& frame, int rotate, int frame_number) {
	switch (rotate) {
		case 1:
			if (frame_number % 3 == 1) {
				cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
			} else if (frame_number % 3 == 2) {
				cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE);
			}
			break;
		case 2: cv::rotate(frame, frame, frame_number % 2 == 0 ? cv::ROTATE_90_COUNTERCLOCKWISE : cv::ROTATE_90_CLOCKWISE); break;
		case ROTATE_FIXED_CCW: cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE); break;
		case ROTATE_FIXED_CW: cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE); break;
		default: break;
	}
}

} // namespace howdy

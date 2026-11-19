#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <dlib/geometry/rectangle.h>
#include <opencv2/core.hpp>

namespace howdy {

// Raised when a model file is missing or can not be loaded
class ModelException : public std::runtime_error {
  public:
	using std::runtime_error::runtime_error;
};

using Encoding = std::vector<double>;

// Face detection and encoding, implemented by dlib and OpenVINO
class FaceBackend {
  public:
	FaceBackend() = default;
	virtual ~FaceBackend() = default;
	FaceBackend(const FaceBackend&) = delete;
	FaceBackend& operator=(const FaceBackend&) = delete;
	FaceBackend(FaceBackend&&) = delete;
	FaceBackend& operator=(FaceBackend&&) = delete;

	// Detect faces in the grayscale frame, upsampling it once
	virtual std::vector<dlib::rectangle> detect(const cv::Mat& gray_frame) = 0;

	// Facial landmarks used for micromotion tracking. The first 5 points have
	// to follow the dlib 5-point layout. An empty result resets the tracking.
	virtual std::vector<cv::Point2f> motion_landmarks(const cv::Mat& frame, const dlib::rectangle& face) = 0;

	// Encode the face, std::nullopt if it can not be encoded
	virtual std::optional<Encoding> encode(const cv::Mat& frame, const dlib::rectangle& face) = 0;

	// Size of a single stored encoding
	[[nodiscard]] virtual std::size_t encoding_size() const = 0;
};

std::unique_ptr<FaceBackend> create_dlib_backend(bool use_cnn);

} // namespace howdy

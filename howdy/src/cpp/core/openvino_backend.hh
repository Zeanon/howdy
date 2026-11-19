#pragma once

#include <memory>
#include <string>

#include <dlib/geometry/rectangle.h>
#include <opencv2/core.hpp>
#include <openvino/openvino.hpp>

#include "face_backend.hh"

namespace howdy {

// Wraps a compiled OpenVINO model with a reusable infer request
class OpenVINOModel {
  public:
	OpenVINOModel(const std::string& file, const char* description, const std::string& device);

	// Run the model on a single NCHW float input and return the first output
	// as float values.
	std::vector<float> infer(const std::vector<float>& input);

	[[nodiscard]] const ov::CompiledModel& compiled() const {
		return compiled_;
	}
	[[nodiscard]] ov::Shape input_shape() const;

  private:
	ov::CompiledModel compiled_;
	ov::InferRequest request_;
};

// Cache the compiled models in this directory instead of the system wide one,
// for tools that run as a regular user. Has to be called before the first
// model is loaded.
void set_openvino_cache_dir(const std::string& directory);

std::unique_ptr<FaceBackend> create_openvino_backend(const std::string& device, bool with_landmarks);

// anti-spoofing-480: spoof probability of the centered square crop of the
// frame. Higher means more likely a spoof.
class SpoofDetector {
  public:
	explicit SpoofDetector(const std::string& device);
	double operator()(const cv::Mat& frame);

  private:
	OpenVINOModel model_;
};

// DAMO FLIR: probability that the face is real
class SpoofDetectorFlir {
  public:
	explicit SpoofDetectorFlir(const std::string& device);
	double operator()(const cv::Mat& frame, const dlib::rectangle& face);

  private:
	OpenVINOModel model_;
};

} // namespace howdy

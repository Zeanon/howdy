#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <opencv2/core.hpp>

#include "config.hh"

namespace howdy {

// Raised when the camera device is not available or can not be read.
class NoDeviceException : public std::runtime_error {
  public:
	using std::runtime_error::runtime_error;
};

// Minimal common interface of the frame sources, modeled after the subset of
// cv::VideoCapture used by Howdy.
class FrameReader {
  public:
	FrameReader() = default;
	virtual ~FrameReader() = default;
	FrameReader(const FrameReader&) = delete;
	FrameReader& operator=(const FrameReader&) = delete;
	FrameReader(FrameReader&&) = delete;
	FrameReader& operator=(FrameReader&&) = delete;

	virtual bool read(cv::Mat& frame) = 0;
	virtual void grab() = 0;
	virtual void release() = 0;
	virtual bool set(int prop, double value) = 0;
	[[nodiscard]] virtual double get(int prop) const = 0;
};

class VideoCapture {
  public:
	explicit VideoCapture(const Config& config);
	~VideoCapture();

	VideoCapture(const VideoCapture&) = delete;
	VideoCapture& operator=(const VideoCapture&) = delete;
	VideoCapture(VideoCapture&&) = delete;
	VideoCapture& operator=(VideoCapture&&) = delete;

	// Returns (color frame, grayscale frame).
	std::pair<cv::Mat, cv::Mat> read_frame();

	void release();

	[[nodiscard]] double get(int prop) const {
		return internal_->get(prop);
	}

  private:
	void create_reader();
	[[nodiscard]] cv::Mat scale_raw(const cv::Mat& frame) const;

	const Config& config_;
	std::unique_ptr<FrameReader> internal_;
	long exposure_ = -1;
	std::optional<double> original_brightness_;
	bool released_ = false;
};

} // namespace howdy

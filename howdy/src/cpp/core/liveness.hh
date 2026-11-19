#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include <dlib/geometry/rectangle.h>
#include <opencv2/core.hpp>

namespace howdy {

// Fixed size FIFO, the equivalent of collections.deque(maxlen=n)
template <typename T> class RingBuffer {
  public:
	explicit RingBuffer(std::size_t maxlen) : maxlen_(maxlen) {
	}

	void push(T value) {
		if (maxlen_ == 0) {
			return;
		}
		if (data_.size() == maxlen_) {
			data_.pop_front();
		}
		data_.push_back(std::move(value));
	}

	void clear() {
		data_.clear();
	}
	[[nodiscard]] std::size_t size() const {
		return data_.size();
	}
	[[nodiscard]] std::size_t maxlen() const {
		return maxlen_;
	}
	[[nodiscard]] bool empty() const {
		return data_.empty();
	}
	[[nodiscard]] const T& back() const {
		return data_.back();
	}
	[[nodiscard]] auto begin() const {
		return data_.begin();
	}
	[[nodiscard]] auto end() const {
		return data_.end();
	}

  private:
	std::size_t maxlen_;
	std::deque<T> data_;
};

// Tracks the nose position over the last frames, normalized by the eye
// distance. Landmarks follow the dlib 5-point layout for the first 5 points
// (two points per eye, then the nose), which the OpenVINO 35-point model
// shares.
class MicroMotionTracker {
  public:
	void init(int buffer_size);
	void reset();

	// Returns the displacement in interocular-distance units. An empty
	// landmark list resets the tracking.
	double calculate_displacement(const std::vector<cv::Point2f>& landmarks);

  private:
	struct Sample {
		cv::Point2f position;
		float scale;
	};
	std::optional<RingBuffer<Sample>> buffer_;
};

// Laplacian variance of the face region, averaged over the last frames.
// Printed photos and screens show a lot less fine texture than a real face.
class TextureDetector {
  public:
	void init(int buffer_size);
	void reset();

	// Returns std::nullopt if the detector is not initialized or the face lies
	// outside of the frame.
	std::optional<double> operator()(const cv::Mat& gray_frame, const dlib::rectangle& face);

  private:
	std::optional<RingBuffer<double>> buffer_;
};

// Compares the encoding with the one of the previous frame. A frozen video
// yields (nearly) identical encodings, a held up photo a sudden change.
class SimilarityDetector {
  public:
	void init(int buffer_size);
	void reset();

	// Returns (similarity to the previous frame, stale)
	std::pair<double, bool> operator()(const std::vector<double>& encoding, double identical_threshold);

  private:
	std::optional<RingBuffer<std::vector<float>>> encodings_;
	std::optional<RingBuffer<double>> similarities_;
};

} // namespace howdy

#include "liveness.hh"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace howdy {

// -----------------------------------------------------------------------------
// MicroMotionTracker
// -----------------------------------------------------------------------------

void MicroMotionTracker::init(int buffer_size) {
	if (buffer_size < 1) {
		throw std::invalid_argument("Micromotion buffersize has to be at least 1");
	}
	buffer_.emplace(static_cast<std::size_t>(buffer_size));
}

void MicroMotionTracker::reset() {
	buffer_.reset();
}

double MicroMotionTracker::calculate_displacement(const std::vector<cv::Point2f>& landmarks) {
	if (!buffer_.has_value()) {
		return 0.0;
	}

	if (landmarks.size() < 5) {
		buffer_->clear();
		return 0.0;
	}

	// right_eye - left_eye
	const cv::Point2f left_eye = (landmarks[0] + landmarks[1]) * 0.5F;
	const cv::Point2f right_eye = (landmarks[2] + landmarks[3]) * 0.5F;
	const auto eye_distance = static_cast<float>(cv::norm(right_eye - left_eye));

	if (eye_distance < 1.0F) {
		buffer_->clear();
		return 0.0;
	}

	// Track the nose position, normalized by eye distance.
	buffer_->push({landmarks[4], eye_distance});

	if (static_cast<double>(buffer_->size()) < static_cast<double>(buffer_->maxlen()) / 2.0) {
		return 0.0;
	}

	const cv::Point2f first = buffer_->begin()->position;
	double max_distance = 0.0;
	double scale_sum = 0.0;
	for (const auto& sample : *buffer_) {
		max_distance = std::max(max_distance, cv::norm(sample.position - first));
		scale_sum += sample.scale;
	}

	return max_distance / (scale_sum / static_cast<double>(buffer_->size()));
}

// -----------------------------------------------------------------------------
// TextureDetector
// -----------------------------------------------------------------------------

void TextureDetector::init(int buffer_size) {
	if (buffer_size < 1) {
		throw std::invalid_argument("Texture buffersize has to be at least 1");
	}
	buffer_.emplace(static_cast<std::size_t>(buffer_size));
}

void TextureDetector::reset() {
	buffer_.reset();
}

std::optional<double> TextureDetector::operator()(const cv::Mat& gray_frame, const dlib::rectangle& face) {
	if (!buffer_.has_value()) {
		return std::nullopt;
	}

	const int width = gray_frame.cols;
	const int height = gray_frame.rows;

	// Clamp ROI to image bounds.
	const int x1 = std::clamp(static_cast<int>(face.left()), 0, width);
	const int x2 = std::clamp(static_cast<int>(face.right()), 0, width);
	const int y1 = std::clamp(static_cast<int>(face.top()), 0, height);
	const int y2 = std::clamp(static_cast<int>(face.bottom()), 0, height);

	if (x2 <= x1 || y2 <= y1) {
		buffer_->clear();
		return std::nullopt;
	}

	// Normalize ROI size so texture statistics are more comparable.
	cv::Mat resized;
	cv::resize(gray_frame(cv::Range(y1, y2), cv::Range(x1, x2)), resized, cv::Size(128, 128), 0, 0, cv::INTER_AREA);

	cv::Mat laplacian;
	cv::Laplacian(resized, laplacian, CV_64F);

	// Population variance, like numpy.var()
	cv::Scalar mean;
	cv::Scalar stddev;
	cv::meanStdDev(laplacian, mean, stddev);
	const double laplacian_var = stddev[0] * stddev[0];

	// Temporal smoothing of the Laplacian measurement.
	buffer_->push(laplacian_var);

	return std::accumulate(buffer_->begin(), buffer_->end(), 0.0) / static_cast<double>(buffer_->size());
}

// -----------------------------------------------------------------------------
// SimilarityDetector
// -----------------------------------------------------------------------------

void SimilarityDetector::init(int buffer_size) {
	if (buffer_size < 2) {
		throw std::invalid_argument("Similarity buffersize has to be at least 2");
	}
	encodings_.emplace(static_cast<std::size_t>(buffer_size));
	similarities_.emplace(static_cast<std::size_t>(buffer_size - 1));
}

void SimilarityDetector::reset() {
	encodings_.reset();
	similarities_.reset();
}

std::pair<double, bool> SimilarityDetector::operator()(const std::vector<double>& encoding, double identical_threshold) {
	if (encoding.empty() || !encodings_.has_value() || !similarities_.has_value()) {
		return {1.0, false};
	}

	std::vector<float> normalized(encoding.begin(), encoding.end());

	if (!std::all_of(normalized.begin(), normalized.end(), [](float v) { return std::isfinite(v); })) {
		return {1.0, false};
	}

	const double norm = std::sqrt(std::inner_product(normalized.begin(), normalized.end(), normalized.begin(), 0.0F));
	if (norm <= 1e-12) {
		return {1.0, false};
	}
	for (auto& v : normalized) {
		v = static_cast<float>(v / norm);
	}

	// Never compare embeddings of different dimensions.
	if (!encodings_->empty() && encodings_->back().size() != normalized.size()) {
		encodings_->clear();
		similarities_->clear();
	}

	std::optional<double> similarity;
	if (!encodings_->empty()) {
		const auto& last = encodings_->back();
		similarity = std::inner_product(last.begin(), last.end(), normalized.begin(), 0.0F);
		similarities_->push(*similarity);
	}

	encodings_->push(std::move(normalized));

	if (!similarity.has_value()) {
		return {1.0, false};
	}

	// Nearly identical encodings over the whole window.
	const bool stale = std::all_of(similarities_->begin(), similarities_->end(), [&](double s) { return s >= identical_threshold; });

	return {*similarity, stale};
}

} // namespace howdy

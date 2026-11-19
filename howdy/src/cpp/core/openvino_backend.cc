#include "openvino_backend.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#include "paths.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr const char* DET_XML = "face-detection-adas-0001.xml";
constexpr const char* REID_XML = "face-reidentification-retail-0095.xml";
constexpr const char* SPOOF_XML = "anti-spoofing-480.xml";
constexpr const char* DAMO_FLIR_XML = "damo-flir.xml";
constexpr const char* LANDMARK_MODEL_XML = "facial-landmarks-35-adas-0002.xml";

// Where the compiled models are cached, set before the first model is loaded
std::string& cache_dir_override() {
	static std::string directory;
	return directory;
}

ov::Core& core() {
	static ov::Core* instance = nullptr;
	static std::once_flag once;
	std::call_once(once, [] {
		// Keep the OpenCL kernel caches out of the (read only) system
		setenv("CL_CACHE_DIR", "/tmp/cl_cache", 0);
		setenv("INTEL_OCL_CACHE_DIR", "/tmp/intel_ocl", 0);

		instance = new ov::Core(); // NOLINT(cppcoreguidelines-owning-memory): lives for the whole process

		const std::filesystem::path cache_dir(cache_dir_override().empty() ? std::string(paths::OPENVINO_CACHE_DIR) : cache_dir_override());
		std::error_code ec;
		if (std::filesystem::create_directories(cache_dir, ec)) {
			std::filesystem::permissions(cache_dir, std::filesystem::perms::owner_all, ec);
		}
		instance->set_property(ov::cache_dir(cache_dir.string()));
	});
	return *instance;
}

// HWC 8 bit or float image to a flat NCHW float buffer
std::vector<float> to_nchw(const cv::Mat& image) {
	cv::Mat floats;
	image.convertTo(floats, CV_32F);
	const int channels = floats.channels();
	const auto plane = static_cast<std::size_t>(floats.rows) * static_cast<std::size_t>(floats.cols);

	std::vector<float> result(plane * static_cast<std::size_t>(channels));
	std::vector<cv::Mat> planes(static_cast<std::size_t>(channels));
	for (int c = 0; c < channels; ++c) {
		planes[static_cast<std::size_t>(c)] = cv::Mat(floats.rows, floats.cols, CV_32FC1, result.data() + (plane * static_cast<std::size_t>(c)));
	}
	cv::split(floats, planes);
	return result;
}

// cv2.dnn.blobFromImage(frame, size=(w, h), ddepth=CV_8U)
std::vector<float> blob_from_image(const cv::Mat& frame, int width, int height) {
	cv::Mat resized;
	cv::resize(frame, resized, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
	return to_nchw(resized);
}

cv::Mat ensure_bgr(const cv::Mat& frame) {
	if (frame.channels() == 1) {
		cv::Mat bgr;
		cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
		return bgr;
	}
	return frame;
}

cv::Rect clamp_rect(const dlib::rectangle& face, int width, int height) {
	const int x1 = std::clamp(static_cast<int>(face.left()), 0, width);
	const int x2 = std::clamp(static_cast<int>(face.right()), 0, width);
	const int y1 = std::clamp(static_cast<int>(face.top()), 0, height);
	const int y2 = std::clamp(static_cast<int>(face.bottom()), 0, height);
	return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

class OpenVINOFaceBackend final : public FaceBackend {
  public:
	OpenVINOFaceBackend(const std::string& device, bool with_landmarks)
		: detector_(DET_XML, "Face-Detector", device), encoder_(REID_XML, "Face-Encoder", device) {
		if (with_landmarks) {
			landmarks_ = std::make_unique<OpenVINOModel>(LANDMARK_MODEL_XML, "Landmark-Model", device);
		}

		const auto det_shape = detector_.input_shape();
		det_h_ = static_cast<int>(det_shape.at(2));
		det_w_ = static_cast<int>(det_shape.at(3));

		const auto enc_shape = encoder_.input_shape();
		enc_h_ = static_cast<int>(enc_shape.at(2));
		enc_w_ = static_cast<int>(enc_shape.at(3));
	}

	std::vector<dlib::rectangle> detect(const cv::Mat& gray_frame) override {
		const int h = gray_frame.rows;
		const int w = gray_frame.cols;
		const auto results = detector_.infer(blob_from_image(ensure_bgr(gray_frame), det_w_, det_h_));

		// [1, 1, N, 7]: image_id, label, conf, x_min, y_min, x_max, y_max
		std::vector<dlib::rectangle> detections;
		for (std::size_t i = 0; i + 7 <= results.size(); i += 7) {
			if (results[i + 2] > 0.5F) {
				const long x1 = std::max(0, static_cast<int>(results[i + 3] * static_cast<float>(w)));
				const long y1 = std::max(0, static_cast<int>(results[i + 4] * static_cast<float>(h)));
				const long x2 = std::min(w, static_cast<int>(results[i + 5] * static_cast<float>(w)));
				const long y2 = std::min(h, static_cast<int>(results[i + 6] * static_cast<float>(h)));
				detections.emplace_back(x1, y1, x2, y2);
			}
		}
		return detections;
	}

	// Idea derived from https://github.com/oktorio/liveness_app/blob/main/liveness_app.py
	std::vector<cv::Point2f> motion_landmarks(const cv::Mat& frame, const dlib::rectangle& face) override {
		if (!landmarks_) {
			return {};
		}

		const cv::Rect roi = clamp_rect(face, frame.cols, frame.rows);
		if (roi.empty()) {
			return {};
		}

		// The model expects a 60x60 RGB input, NCHW, float32, no normalization.
		cv::Mat face_rgb;
		cv::cvtColor(ensure_bgr(frame)(roi), face_rgb, cv::COLOR_BGR2RGB);
		cv::resize(face_rgb, face_rgb, cv::Size(60, 60), 0, 0, cv::INTER_LINEAR);

		const auto output = landmarks_->infer(to_nchw(face_rgb));
		if (output.size() < 70) {
			return {};
		}

		// Convert normalized face-crop coordinates to frame coordinates.
		std::vector<cv::Point2f> points(35);
		for (std::size_t i = 0; i < points.size(); ++i) {
			points[i].x = static_cast<float>(roi.x) + (output[2 * i] * static_cast<float>(roi.width));
			points[i].y = static_cast<float>(roi.y) + (output[(2 * i) + 1] * static_cast<float>(roi.height));
		}
		return points;
	}

	std::optional<Encoding> encode(const cv::Mat& frame, const dlib::rectangle& face) override {
		const cv::Mat bgr = ensure_bgr(frame);
		const int x1 = std::max(0, static_cast<int>(face.left()));
		const int y1 = std::max(0, static_cast<int>(face.top()));
		const int x2 = std::min(bgr.cols, static_cast<int>(face.right()));
		const int y2 = std::min(bgr.rows, static_cast<int>(face.bottom()));
		if (x2 <= x1 || y2 <= y1) {
			return std::nullopt;
		}

		auto embedding = encoder_.infer(blob_from_image(bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1)), enc_w_, enc_h_));

		const float norm = std::sqrt(std::inner_product(embedding.begin(), embedding.end(), embedding.begin(), 0.0F));
		if (norm > 0) {
			for (auto& v : embedding) {
				v /= norm;
			}
		}
		return Encoding(embedding.begin(), embedding.end());
	}

	[[nodiscard]] std::size_t encoding_size() const override {
		return 256;
	}

  private:
	OpenVINOModel detector_;
	OpenVINOModel encoder_;
	std::unique_ptr<OpenVINOModel> landmarks_;
	int det_w_ = 0;
	int det_h_ = 0;
	int enc_w_ = 0;
	int enc_h_ = 0;
};

} // namespace

// -----------------------------------------------------------------------------
// OpenVINOModel
// -----------------------------------------------------------------------------

OpenVINOModel::OpenVINOModel(const std::string& file, const char* description, const std::string& device) {
	const auto path = std::filesystem::path(paths::OPENVINO_DATA_DIR) / file;
	if (!std::filesystem::is_regular_file(path)) {
		throw ModelException(std::string("OpenVINO ") + description + " model does not exist");
	}

	auto& ov_core = core();
	compiled_ = ov_core.compile_model(ov_core.read_model(path.string()), device);
	request_ = compiled_.create_infer_request();
}

ov::Shape OpenVINOModel::input_shape() const {
	return compiled_.input(0).get_shape();
}

std::vector<float> OpenVINOModel::infer(const std::vector<float>& input) {
	const auto port = compiled_.input(0);
	ov::Tensor tensor(port.get_element_type(), port.get_shape());
	if (tensor.get_size() != input.size()) {
		throw std::runtime_error("OpenVINO input size mismatch: expected " + std::to_string(tensor.get_size()) + ", got " + std::to_string(input.size()));
	}

	// The Python API converts numpy inputs to the model's element type
	// implicitly, do the same here.
	const auto type = port.get_element_type();
	if (type == ov::element::f32) {
		std::copy(input.begin(), input.end(), tensor.data<float>());
	} else if (type == ov::element::f16) {
		std::transform(input.begin(), input.end(), tensor.data<ov::float16>(), [](float v) { return ov::float16(v); });
	} else if (type == ov::element::u8) {
		std::transform(input.begin(), input.end(), tensor.data<std::uint8_t>(), [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); });
	} else {
		throw std::runtime_error("Unsupported OpenVINO input element type: " + type.get_type_name());
	}

	request_.set_input_tensor(tensor);
	request_.infer();

	const auto output = request_.get_output_tensor(0);
	const auto out_type = output.get_element_type();
	std::vector<float> result(output.get_size());
	if (out_type == ov::element::f32) {
		std::copy_n(output.data<const float>(), result.size(), result.begin());
	} else if (out_type == ov::element::f16) {
		const auto* data = output.data<const ov::float16>();
		for (std::size_t i = 0; i < result.size(); ++i) {
			result[i] = static_cast<float>(data[i]);
		}
	} else {
		throw std::runtime_error("Unsupported OpenVINO output element type: " + out_type.get_type_name());
	}
	return result;
}

void set_openvino_cache_dir(const std::string& directory) {
	cache_dir_override() = directory;
}

std::unique_ptr<FaceBackend> create_openvino_backend(const std::string& device, bool with_landmarks) {
	return std::make_unique<OpenVINOFaceBackend>(device, with_landmarks);
}

// -----------------------------------------------------------------------------
// SpoofDetector
// -----------------------------------------------------------------------------

SpoofDetector::SpoofDetector(const std::string& device) : model_(SPOOF_XML, "Spoof-Detector", device) {
}

double SpoofDetector::operator()(const cv::Mat& frame) {
	// Centered square crop with the length of the shorter side, so landscape,
	// portrait (rotate = 1/2) and square frames are all handled the same way.
	const int side = std::min(frame.rows, frame.cols);
	const int x0 = (frame.cols - side) / 2;
	const int y0 = (frame.rows - side) / 2;

	cv::Mat crop;
	cv::resize(ensure_bgr(frame)(cv::Rect(x0, y0, side, side)), crop, cv::Size(480, 480), 0, 0, cv::INTER_LINEAR);

	// ImageNet normalization, applied to the channels in frame order like the
	// Python implementation does.
	cv::Mat normalized;
	crop.convertTo(normalized, CV_32FC3, 1.0 / 255.0);
	cv::subtract(normalized, cv::Scalar(0.485, 0.456, 0.406), normalized);
	cv::divide(normalized, cv::Scalar(0.229, 0.224, 0.225), normalized);

	const auto output = model_.infer(to_nchw(normalized));
	if (output.empty()) {
		throw std::runtime_error("Spoof-Detector returned no output");
	}

	// TODO Tuning [1.25 can be used to adjust the certainty spread]
	return 1.0 / (1.0 + std::exp(-static_cast<double>(output[0]) / 1.25));
}

// -----------------------------------------------------------------------------
// SpoofDetectorFlir
// -----------------------------------------------------------------------------

SpoofDetectorFlir::SpoofDetectorFlir(const std::string& device) : model_(DAMO_FLIR_XML, "Damo-Flir", device) {
	const auto shape = model_.compiled().input(0).get_partial_shape();
	if (!shape.rank().is_static() || shape.rank().get_length() != 4) {
		throw std::runtime_error("Expected NCHW input, got " + shape.to_string());
	}

	const auto dim = [&](std::ptrdiff_t i, long fallback) { return shape[i].is_static() ? static_cast<long>(shape[i].get_length()) : fallback; };
	const long channels = dim(1, 1);
	const long height = dim(2, 112);
	const long width = dim(3, 112);

	if (width != 112 || height != 112) {
		throw std::runtime_error("DAMO FLIR preprocessing produces 112x112, but model expects " + std::to_string(width) + "x" + std::to_string(height));
	}
	if (channels != 3) {
		// The preprocessing always produces 3 channels
		throw std::runtime_error("Unsupported input channel count: " + std::to_string(channels));
	}
}

double SpoofDetectorFlir::operator()(const cv::Mat& frame, const dlib::rectangle& face) {
	cv::Mat img;
	cv::cvtColor(ensure_bgr(frame), img, cv::COLOR_BGR2RGB);

	const int h = img.rows;
	const int w = img.cols;
	auto x1 = static_cast<int>(face.left());
	auto y1 = static_cast<int>(face.top());
	auto x2 = static_cast<int>(face.right());
	auto y2 = static_cast<int>(face.bottom());

	const int box_w = x2 - x1 + 1;
	const int box_h = y2 - y1 + 1;

	x1 = std::max(0, x1 - (box_w * 16 / 112));
	x2 = std::min(w - 1, x2 + (box_w * 16 / 112));
	y1 = std::max(0, y1 - (box_h * 16 / 112));
	y2 = std::min(h - 1, y2 + (box_h * 16 / 112));

	const int padding_h = y2 - y1 + 1;
	const int padding_w = x2 - x1 + 1;

	int dst_size = 0;
	if (padding_w > padding_h) {
		const int offset = (padding_w - padding_h) / 2;
		y1 -= offset;
		y2 = y1 + padding_w - 1;
		y1 = std::max(0, y1);
		y2 = std::min(h - 1, y2);
		dst_size = padding_w;
	} else {
		const int offset = (padding_h - padding_w) / 2;
		x1 -= offset;
		x2 = x1 + padding_h - 1;
		x1 = std::max(0, x1);
		x2 = std::min(w - 1, x2);
		dst_size = padding_h;
	}

	if (dst_size <= 0 || x2 < x1 || y2 < y1) {
		// Face completely outside of the frame, treat as spoof
		return 0.0;
	}

	cv::Mat dst(dst_size, dst_size, CV_8UC3, cv::Scalar(127, 127, 127));
	const int dst_x_offset = (dst_size - (x2 - x1 + 1)) / 2;
	const int dst_y_offset = (dst_size - (y2 - y1 + 1)) / 2;
	img(cv::Rect(x1, y1, x2 - x1 + 1, y2 - y1 + 1)).copyTo(dst(cv::Rect(dst_x_offset, dst_y_offset, x2 - x1 + 1, y2 - y1 + 1)));

	cv::Mat resized;
	cv::resize(dst, resized, cv::Size(128, 128), 0, 0, cv::INTER_LINEAR);

	cv::Mat normalized;
	resized(cv::Rect(8, 8, 112, 112)).convertTo(normalized, CV_32FC3, 0.0078125, -127.5 * 0.0078125);

	const auto output = model_.infer(to_nchw(normalized));

	// DAMO FLIR: class 0 is spoof, class 1 is real
	if (output.size() == 1) {
		// A single output's meaning depends on the exported model.
		throw std::runtime_error("Model returned one value; verify whether it is a spoof logit, probability, or another score.");
	}
	if (output.size() != 2) {
		throw std::runtime_error("Expected two class scores, got " + std::to_string(output.size()));
	}

	// Preserve probabilities if already normalized; otherwise treat the values
	// as logits and apply softmax.
	const bool probabilities =
		std::all_of(output.begin(), output.end(), [](float v) { return v >= 0 && v <= 1; }) && std::abs((output[0] + output[1]) - 1.0F) <= 1e-3F;
	if (probabilities) {
		return output[1];
	}

	// TODO Tuning
	const std::array<double, 2> values{output[0] / 2.0, output[1] / 2.0};
	const double max_value = std::max(values[0], values[1]);
	const std::array<double, 2> exps{std::exp(values[0] - max_value), std::exp(values[1] - max_value)};
	return exps[1] / (exps[0] + exps[1]);
}

} // namespace howdy

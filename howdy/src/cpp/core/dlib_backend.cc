// dlib face detection, landmarks and encoding.
//
// The network definitions are the ones of dlib's dnn_mmod_face_detection_ex.cpp
// and dnn_face_recognition_ex.cpp, which the model files were trained with.
// The pre- and postprocessing mirrors dlib's Python bindings
// (tools/python/src/*.cpp), so descriptors are identical to the ones the
// Python daemon and `howdy add` produce.

#include "face_backend.hh"

#include <filesystem>

#include <dlib/dnn.h>
#include <dlib/image_processing.h>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_transforms.h>

#include "liveness.hh"
#include "paths.hh"
#include "util.hh"

namespace howdy {

namespace {

// NOLINTBEGIN(readability-identifier-naming)

// --- dnn_mmod_face_detection_ex.cpp ------------------------------------------
template <long num_filters, typename SUBNET> using con5d = dlib::con<num_filters, 5, 5, 2, 2, SUBNET>;
template <long num_filters, typename SUBNET> using con5 = dlib::con<num_filters, 5, 5, 1, 1, SUBNET>;

template <typename SUBNET>
using downsampler = dlib::relu<dlib::affine<con5d<32, dlib::relu<dlib::affine<con5d<32, dlib::relu<dlib::affine<con5d<16, SUBNET>>>>>>>>>;
template <typename SUBNET> using rcon5 = dlib::relu<dlib::affine<con5<45, SUBNET>>>;

using mmod_net_type = dlib::loss_mmod<dlib::con<1, 9, 9, 1, 1, rcon5<rcon5<rcon5<downsampler<dlib::input_rgb_image_pyramid<dlib::pyramid_down<6>>>>>>>>;

// --- dnn_face_recognition_ex.cpp ---------------------------------------------
template <template <int, template <typename> class, int, typename> class block, int N, template <typename> class BN, typename SUBNET>
using residual = dlib::add_prev1<block<N, BN, 1, dlib::tag1<SUBNET>>>;

template <template <int, template <typename> class, int, typename> class block, int N, template <typename> class BN, typename SUBNET>
using residual_down = dlib::add_prev2<dlib::avg_pool<2, 2, 2, 2, dlib::skip1<dlib::tag2<block<N, BN, 2, dlib::tag1<SUBNET>>>>>>;

template <int N, template <typename> class BN, int stride, typename SUBNET>
using block = BN<dlib::con<N, 3, 3, 1, 1, dlib::relu<BN<dlib::con<N, 3, 3, stride, stride, SUBNET>>>>>;

template <int N, typename SUBNET> using ares = dlib::relu<residual<block, N, dlib::affine, SUBNET>>;
template <int N, typename SUBNET> using ares_down = dlib::relu<residual_down<block, N, dlib::affine, SUBNET>>;

template <typename SUBNET> using alevel0 = ares_down<256, SUBNET>;
template <typename SUBNET> using alevel1 = ares<256, ares<256, ares_down<256, SUBNET>>>;
template <typename SUBNET> using alevel2 = ares<128, ares<128, ares_down<128, SUBNET>>>;
template <typename SUBNET> using alevel3 = ares<64, ares<64, ares<64, ares_down<64, SUBNET>>>>;
template <typename SUBNET> using alevel4 = ares<32, ares<32, ares<32, SUBNET>>>;

using anet_type = dlib::loss_metric<dlib::fc_no_bias<
	128, dlib::avg_pool_everything<alevel0<alevel1<
			 alevel2<alevel3<alevel4<dlib::max_pool<3, 3, 2, 2, dlib::relu<dlib::affine<dlib::con<32, 7, 7, 2, 2, dlib::input_rgb_image_sized<150>>>>>>>>>>>>>;

// NOLINTEND(readability-identifier-naming)

constexpr const char* SHAPE_PREDICTOR_MODEL = "shape_predictor_5_face_landmarks.dat";
constexpr const char* CNN_DETECTOR_MODEL = "mmod_human_face_detector.dat";
constexpr const char* RECOGNITION_MODEL = "dlib_face_recognition_resnet_model_v1.dat";

constexpr int UPSAMPLE = 1;

std::string model_path(const char* name, const char* description) {
	auto path = std::filesystem::path(paths::DLIB_DATA_DIR) / name;
	if (!std::filesystem::is_regular_file(path)) {
		throw ModelException(std::string("DLib ") + description + " model does not exist");
	}
	return path.string();
}

// Copy a grayscale cv::Mat into a dlib image
dlib::matrix<unsigned char> to_dlib_gray(const cv::Mat& gray) {
	CV_Assert(gray.type() == CV_8UC1);
	dlib::matrix<unsigned char> image(gray.rows, gray.cols);
	for (int y = 0; y < gray.rows; ++y) {
		const auto* row = gray.ptr<unsigned char>(y);
		for (int x = 0; x < gray.cols; ++x) {
			image(y, x) = row[x];
		}
	}
	return image;
}

// Copy a 3 channel cv::Mat into a dlib rgb image *without* swapping the
// channels. The Python daemon hands the BGR frame from OpenCV straight to
// dlib, which reads it as RGB. Doing the same keeps descriptors compatible
// with already enrolled models.
dlib::matrix<dlib::rgb_pixel> to_dlib_rgb_as_is(const cv::Mat& frame) {
	CV_Assert(frame.type() == CV_8UC3);
	dlib::matrix<dlib::rgb_pixel> image(frame.rows, frame.cols);
	for (int y = 0; y < frame.rows; ++y) {
		const auto* row = frame.ptr<cv::Vec3b>(y);
		for (int x = 0; x < frame.cols; ++x) {
			image(y, x) = dlib::rgb_pixel(row[x][0], row[x][1], row[x][2]);
		}
	}
	return image;
}

class DlibBackend final : public FaceBackend {
  public:
	explicit DlibBackend(bool use_cnn) : use_cnn_(use_cnn) {
		if (use_cnn_) {
			const auto path = model_path(CNN_DETECTOR_MODEL, "Face-Detector");
			dlib::deserialize(path) >> cnn_detector_;
		} else {
			hog_detector_ = dlib::get_frontal_face_detector();
		}

		dlib::deserialize(model_path(SHAPE_PREDICTOR_MODEL, "Landmark-Model")) >> pose_predictor_;
		dlib::deserialize(model_path(RECOGNITION_MODEL, "Face-Encoder")) >> face_encoder_;
	}

	std::vector<dlib::rectangle> detect(const cv::Mat& gray_frame) override {
		last_shape_.reset();
		return use_cnn_ ? detect_cnn(gray_frame) : detect_hog(gray_frame);
	}

	std::vector<cv::Point2f> motion_landmarks(const cv::Mat& frame, const dlib::rectangle& face) override {
		const auto& shape = shape_for(frame, face);
		std::vector<cv::Point2f> points;
		points.reserve(shape.num_parts());
		for (unsigned long i = 0; i < shape.num_parts(); ++i) {
			points.emplace_back(static_cast<float>(shape.part(i).x()), static_cast<float>(shape.part(i).y()));
		}
		return points;
	}

	std::optional<Encoding> encode(const cv::Mat& frame, const dlib::rectangle& face) override {
		// face_recognition_model_v1.compute_face_descriptor(img, shape, num_jitters=1)
		const auto& shape = shape_for(frame, face);

		std::vector<dlib::chip_details> chips{dlib::get_face_chip_details(shape, 150, 0.25)};
		dlib::array<dlib::matrix<dlib::rgb_pixel>> face_chips;
		dlib::extract_image_chips(last_image_, chips, face_chips);

		std::vector<dlib::matrix<dlib::rgb_pixel>> batch(face_chips.begin(), face_chips.end());
		const auto descriptors = face_encoder_(batch, 16);
		if (descriptors.empty()) {
			return std::nullopt;
		}

		const auto& descriptor = descriptors.front();
		Encoding encoding(descriptor.size());
		for (long i = 0; i < descriptor.size(); ++i) {
			encoding[static_cast<std::size_t>(i)] = static_cast<double>(descriptor(i));
		}
		return encoding;
	}

	[[nodiscard]] std::size_t encoding_size() const override {
		return 128;
	}

  private:
	// simple_object_detector_py.h: run_detector_with_upscale1()
	std::vector<dlib::rectangle> detect_hog(const cv::Mat& gray_frame) {
		dlib::pyramid_down<2> pyr;
		dlib::matrix<unsigned char> upsampled;
		dlib::pyramid_up(to_dlib_gray(gray_frame), upsampled, pyr);
		for (int level = 1; level < UPSAMPLE; ++level) {
			dlib::pyramid_up(upsampled, pyr);
		}

		std::vector<dlib::rect_detection> detections;
		hog_detector_(upsampled, detections, 0.0);

		std::vector<dlib::rectangle> rects;
		rects.reserve(detections.size());
		for (const auto& detection : detections) {
			rects.push_back(pyr.rect_down(detection.rect, UPSAMPLE));
		}
		return rects;
	}

	// cnn_face_detector.cpp: cnn_face_detection_model_v1.detect()
	std::vector<dlib::rectangle> detect_cnn(const cv::Mat& gray_frame) {
		dlib::pyramid_down<2> pyr;
		dlib::matrix<dlib::rgb_pixel> image;
		dlib::assign_image(image, to_dlib_gray(gray_frame));
		for (int level = 0; level < UPSAMPLE; ++level) {
			dlib::pyramid_up(image, pyr);
		}

		std::vector<dlib::rectangle> rects;
		for (const auto& detection : cnn_detector_(image)) {
			rects.push_back(pyr.rect_down(detection.rect, UPSAMPLE));
		}
		return rects;
	}

	const dlib::full_object_detection& shape_for(const cv::Mat& frame, const dlib::rectangle& face) {
		if (!last_shape_.has_value() || last_shape_->get_rect() != face || last_frame_data_ != frame.data) {
			last_image_ = to_dlib_rgb_as_is(frame);
			last_frame_data_ = frame.data;
			last_shape_ = pose_predictor_(last_image_, face);
		}
		return *last_shape_;
	}

	bool use_cnn_;
	dlib::frontal_face_detector hog_detector_;
	mmod_net_type cnn_detector_;
	dlib::shape_predictor pose_predictor_;
	anet_type face_encoder_;

	// Landmarks are needed for micromotion and encoding of the same face,
	// compute them only once.
	dlib::matrix<dlib::rgb_pixel> last_image_;
	const unsigned char* last_frame_data_ = nullptr;
	std::optional<dlib::full_object_detection> last_shape_;
};

} // namespace

std::unique_ptr<FaceBackend> create_dlib_backend(bool use_cnn) {
	return std::make_unique<DlibBackend>(use_cnn);
}

} // namespace howdy

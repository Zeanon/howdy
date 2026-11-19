// Parity harness for the C++ daemon: prints detections, landmarks, encodings
// and liveness/spoof scores as JSON, to be compared with parity.py.
//
// Usage: parity <hog|cnn|openvino> <images...>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "face_backend.hh"
#include "liveness.hh"
#ifdef HOWDY_WITH_OPENVINO
#include "openvino_backend.hh"
#endif

using nlohmann::json;

int main(int argc, char** argv) {
	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <hog|cnn|openvino> <images...>\n";
		return 2;
	}
	const std::string mode = argv[1];
	json out = json::object();

	std::unique_ptr<howdy::FaceBackend> backend;
#ifdef HOWDY_WITH_OPENVINO
	std::unique_ptr<howdy::SpoofDetector> spoof;
	std::unique_ptr<howdy::SpoofDetectorFlir> flir;
#endif
	if (mode == "hog")
		backend = howdy::create_dlib_backend(false);
	if (mode == "cnn")
		backend = howdy::create_dlib_backend(true);
#ifdef HOWDY_WITH_OPENVINO
	if (mode == "openvino") {
		backend = howdy::create_openvino_backend("CPU", true);
		spoof = std::make_unique<howdy::SpoofDetector>("CPU");
		flir = std::make_unique<howdy::SpoofDetectorFlir>("CPU");
	}
#endif
	if (!backend) {
		std::cerr << "Unsupported mode: " << mode << "\n";
		return 2;
	}

	for (int i = 2; i < argc; ++i) {
		cv::Mat frame = cv::imread(argv[i], cv::IMREAD_COLOR);
		cv::Mat gray;
		cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
		json img = json::object();
#ifdef HOWDY_WITH_OPENVINO
		if (spoof) {
			img["spoof"] = (*spoof)(frame);
			cv::Mat portrait;
			cv::rotate(frame, portrait, cv::ROTATE_90_CLOCKWISE);
			img["spoof_portrait"] = (*spoof)(portrait);
		}
#endif
		json faces = json::array();
		for (const auto& r : backend->detect(gray)) {
			json f;
			f["rect"] = {r.left(), r.top(), r.right(), r.bottom()};
			howdy::TextureDetector texture;
			texture.init(1);
			const auto t = texture(gray, r);
			f["texture"] = t.has_value() ? json(*t) : json(nullptr);
			const auto lm = backend->motion_landmarks(frame, r);
			json pts = json::array();
			for (std::size_t k = 0; k < std::min<std::size_t>(5, lm.size()); ++k)
				pts.push_back({lm[k].x, lm[k].y});
			f["landmarks"] = pts;
			const auto enc = backend->encode(frame, r);
			f["encoding"] = enc.has_value() ? json(*enc) : json(nullptr);
#ifdef HOWDY_WITH_OPENVINO
			if (flir)
				f["flir"] = (*flir)(frame, r);
#endif
			faces.push_back(f);
		}
		img["faces"] = faces;
		out[argv[i]] = img;
	}
	std::cout << out.dump() << std::endl;
	return 0;
}

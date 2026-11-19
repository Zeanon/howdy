#include "face_setup.hh"

#include <filesystem>

#include "paths.hh"
#include "util.hh"

#ifdef HOWDY_WITH_OPENVINO
#include "openvino_backend.hh"
#endif

namespace howdy {

std::unique_ptr<FaceBackend> create_face_backend(const Config& config, const std::string& user, const std::function<void(const std::string&)>& say) {
	const bool use_cnn = config.get_bool("face_authentication", "use_cnn", false);
	const bool use_openvino = config.get_bool("face_authentication", "use_openvino", false);

	if (use_openvino) {
#ifdef HOWDY_WITH_OPENVINO
		const std::string device = to_upper(config.get("video", "openvino_device", "GPU"));
		try {
			const auto home = lookup_home(user);
			if (home.has_value()) {
				const std::filesystem::path cache = std::filesystem::path(*home) / ".cache" / "howdy" / "openvino";
				std::error_code ec;
				std::filesystem::create_directories(cache, ec);
				if (!ec) {
					set_openvino_cache_dir(cache.string());
				}
			}

			auto backend = create_openvino_backend(device, false);
			if (say) {
				say("Using OpenVINO " + device + " for face detection and encoding");
			}
			return backend;
		} catch (const ModelException& exc) {
			std::string reason = std::string("OpenVINO init failed: '") + exc.what() + "'\n";
			reason += std::string(S("OpenVINO data files have not been downloaded, please run the following commands:")) + "\n";
			reason += std::string("\tcd ") + paths::OPENVINO_DATA_DIR + "\n\tsudo ./install.sh";
			throw std::runtime_error(reason);
		} catch (const std::exception& exc) {
			throw std::runtime_error(std::string("OpenVINO not available (") + exc.what() + ")");
		}
#else
		throw std::runtime_error("OpenVINO not available (howdy was built without OpenVINO support)");
#endif
	}

	try {
		return create_dlib_backend(use_cnn);
	} catch (const ModelException& exc) {
		std::string reason = std::string(exc.what()) + "\n";
		reason += std::string(S("DLIB data files have not been downloaded, please run the following commands:")) + "\n";
		reason += std::string("\tcd ") + paths::DLIB_DATA_DIR + "\n\tsudo ./install.sh";
		throw std::runtime_error(reason);
	}
}

} // namespace howdy

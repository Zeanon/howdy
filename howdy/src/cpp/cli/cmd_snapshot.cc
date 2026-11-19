// howdy snapshot: save a few frames of the camera to the snapshot directory

#include <array>
#include <ctime>
#include <filesystem>
#include <iostream>

#include "cli.hh"
#include "config.hh"
#include "paths.hh"
#include "snapshot.hh"
#include "util.hh"
#include "video_capture.hh"

namespace howdy::cli {

int run_snapshot(const Options& /*options*/) {
	const Config config((std::filesystem::path(paths::CONFIG_DIR) / "config.ini").string());

	// Start video capture
	std::unique_ptr<VideoCapture> video_capture;
	try {
		video_capture = std::make_unique<VideoCapture>(config);
		// Read a frame to activate emitters
		video_capture->read_frame();
	} catch (const NoDeviceException& exc) {
		throw CliError(exc.what(), EXIT_INVALID_DEVICE);
	}

	// Collection of recorded frames
	std::vector<cv::Mat> frames;
	try {
		while (frames.size() < 4) {
			frames.push_back(video_capture->read_frame().first);
		}
	} catch (const NoDeviceException& exc) {
		throw CliError(exc.what(), EXIT_INVALID_DEVICE);
	}
	video_capture.reset();

	const std::time_t now = std::time(nullptr);
	std::tm utc{};
	gmtime_r(&now, &utc);
	std::array<char, 32> date{};
	std::strftime(date.data(), date.size(), "%Y/%m/%d %H:%M:%S UTC", &utc);

	// Generate a snapshot image from the frames
	std::filesystem::path file;
	try {
		file = generate_snapshot(
			frames, {
						S("GENERATED SNAPSHOT"),
						std::string(S("Date: ")) + date.data(),
						std::string(S("Dark threshold config: ")) + format_double(config.get_float("video", "dark_threshold", 60.0)),
						std::string(S("Certainty config: ")) + format_double(config.get_float("face_authentication", "certainty", 75.0)),
					});
	} catch (const std::exception& exc) {
		throw CliError(exc.what());
	}

	// Show the file location in console
	std::cout << S("Generated snapshot saved as") << "\n" << file.string() << '\n';
	return 0;
}

} // namespace howdy::cli

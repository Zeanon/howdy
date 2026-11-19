#include "snapshot.hh"

#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "paths.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr int LOGO_WIDTH = 180;
constexpr int LOGO_HEIGHT = 57;

std::string timestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm utc{};
	gmtime_r(&now, &utc);
	std::array<char, 32> buffer{};
	std::strftime(buffer.data(), buffer.size(), "%Y%m%dT%H%M%S", &utc);
	return buffer.data();
}

} // namespace

std::filesystem::path generate_snapshot(const std::vector<cv::Mat>& frames, const std::vector<std::string>& text_lines) {
	// Don't execute if no frames were given
	if (frames.empty()) {
		return {};
	}

	const int frame_height = frames[0].rows;
	const int frame_width = frames[0].cols;

	// Spread the given frames out horizontally
	cv::Mat snap;
	cv::hconcat(frames, snap);

	// Add a gray square at the bottom of the image
	const cv::Scalar pad_color(44, 44, 44);
	const cv::Scalar text_color(255, 255, 255);
	cv::copyMakeBorder(snap, snap, 0, (static_cast<int>(text_lines.size()) * 20) + 40, 0, 0, cv::BORDER_CONSTANT, pad_color);

	// Add the Howdy logo if there's space to do so
	if (frames.size() > 1) {
		cv::Mat logo = cv::imread((std::filesystem::path(paths::DATA_DIR) / "assets/logo.png").string(), cv::IMREAD_COLOR);
		const int logo_y = frame_height + 20;
		const int logo_x = (frame_width * static_cast<int>(frames.size())) - 210;
		if (!logo.empty() && logo_x >= 0 && logo_y + LOGO_HEIGHT <= snap.rows) {
			if (logo.cols != LOGO_WIDTH || logo.rows != LOGO_HEIGHT) {
				cv::resize(logo, logo, cv::Size(LOGO_WIDTH, LOGO_HEIGHT), 0, 0, cv::INTER_AREA);
			}
			logo.copyTo(snap(cv::Rect(logo_x, logo_y, LOGO_WIDTH, LOGO_HEIGHT)));
		}
	}

	// Print every line onto the image
	int line_number = 0;
	for (const auto& line : text_lines) {
		const int padding_top = frame_height + 30 + (line_number * 20);
		cv::putText(snap, line, cv::Point(30, padding_top), cv::FONT_HERSHEY_SIMPLEX, 0.4, text_color, 1, cv::LINE_AA);
		++line_number;
	}

	// Create snapshot directory with owner-only permissions
	const std::filesystem::path directory = std::filesystem::path(paths::LOG_PATH) / "snapshots";
	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	if (ec) {
		throw std::runtime_error("Could not create " + directory.string() + ": " + ec.message());
	}
	chmod(directory.c_str(), 0700);

	std::vector<unsigned char> encoded;
	if (!cv::imencode(".jpg", snap, encoded)) {
		throw std::runtime_error("Failed to encode snapshot");
	}

	// Create snapshot file with owner-only permissions, never overwrite one
	const std::filesystem::path path = directory / (timestamp() + ".jpg");
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("Could not create " + path.string() + ": " + std::strerror(errno));
	}

	// However this function is left: the file is closed and a file that could not
	// be written completely is removed
	bool complete = false;
	const ScopeExit cleanup([&] {
		close(fd);
		if (!complete) {
			unlink(path.c_str());
		}
	});

	for (std::size_t written = 0; written < encoded.size();) {
		const ssize_t n = ::write(fd, encoded.data() + written, encoded.size() - written);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error("Could not write " + path.string() + ": " + std::strerror(errno));
		}
		written += static_cast<std::size_t>(n);
	}
	complete = true;

	return path;
}

} // namespace howdy

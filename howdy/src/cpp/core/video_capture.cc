#include "video_capture.hh"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <regex>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "util.hh"

namespace howdy {

namespace {

// -----------------------------------------------------------------------------
// OpenCV reader
// -----------------------------------------------------------------------------

class OpenCVReader final : public FrameReader {
  public:
	explicit OpenCVReader(const std::string& device_path) {
		capture_.open(device_path, cv::CAP_V4L);
		if (!capture_.isOpened()) {
			// Allow "/dev/video2" style paths to fall back to the device index, also behind a link of
			// /dev/v4l. The number at the end of another path ("...-video-index0") is not that index, it
			// would open another camera.
			static const std::regex VIDEO_NODE(R"(^/dev/video(\d+)$)");
			std::error_code ec;
			const std::string node = std::filesystem::weakly_canonical(device_path, ec).string();
			std::smatch match;
			if (!ec && std::regex_match(node, match, VIDEO_NODE)) {
				capture_.open(std::stoi(match[1].str()));
			}
		}
	}

	bool read(cv::Mat& frame) override {
		return capture_.read(frame);
	}
	void grab() override {
		capture_.grab();
	}
	void release() override {
		capture_.release();
	}
	bool set(int prop, double value) override {
		return capture_.set(prop, value);
	}
	[[nodiscard]] double get(int prop) const override {
		return capture_.get(prop);
	}

  private:
	// cv::VideoCapture::get() is not const
	mutable cv::VideoCapture capture_;
};

// -----------------------------------------------------------------------------
// ffmpeg reader
// -----------------------------------------------------------------------------

struct ProcessResult {
	int exit_code = -1;
	std::string out;
	std::string err;
};

// How long ffmpeg may take: a camera that delivers nothing would hold the daemon for good
constexpr auto PROCESS_TIMEOUT = std::chrono::seconds(10);

// Run a program without a shell and collect stdout and stderr, it is ended after PROCESS_TIMEOUT.
ProcessResult run_process(const std::vector<std::string>& args) {
	// Read end [0] and write end [1] of the pipes, -1 marks an end that is closed
	std::array<int, 2> out_pipe{};
	std::array<int, 2> err_pipe{};
	const auto close_end = [](int& fd) {
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	};

	if (pipe2(out_pipe.data(), O_CLOEXEC) != 0) {
		throw NoDeviceException(std::string("pipe failed: ") + std::strerror(errno));
	}
	const ScopeExit close_out_pipe([&] {
		close_end(out_pipe[0]);
		close_end(out_pipe[1]);
	});

	if (pipe2(err_pipe.data(), O_CLOEXEC) != 0) {
		throw NoDeviceException(std::string("pipe failed: ") + std::strerror(errno));
	}
	const ScopeExit close_err_pipe([&] {
		close_end(err_pipe[0]);
		close_end(err_pipe[1]);
	});

	std::vector<char*> argv;
	argv.reserve(args.size() + 1);
	for (const auto& arg : args) {
		argv.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
	}
	argv.push_back(nullptr);

	const pid_t pid = fork();
	if (pid < 0) {
		throw NoDeviceException(std::string("fork failed: ") + std::strerror(errno));
	}

	if (pid == 0) {
		const int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
		}
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(err_pipe[1], STDERR_FILENO);
		execvp(argv[0], argv.data());
		_exit(127);
	}

	close_end(out_pipe[1]);
	close_end(err_pipe[1]);

	ProcessResult result;
	std::array<int*, 2> read_ends{out_pipe.data(), err_pipe.data()};
	std::array<pollfd, 2> fds{{{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}}};
	std::array<std::string*, 2> targets{&result.out, &result.err};
	std::array<char, 65536> buffer{};
	int open_fds = 2;

	const auto deadline = std::chrono::steady_clock::now() + PROCESS_TIMEOUT;
	bool timed_out = false;
	while (open_fds > 0) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (left.count() <= 0) {
			timed_out = true;
			kill(pid, SIGKILL);
			break;
		}
		if (poll(fds.data(), fds.size(), static_cast<int>(left.count())) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		for (std::size_t i = 0; i < fds.size(); ++i) {
			if (fds.at(i).fd < 0 || fds.at(i).revents == 0) {
				continue;
			}
			const ssize_t n = ::read(fds.at(i).fd, buffer.data(), buffer.size());
			if (n > 0) {
				targets.at(i)->append(buffer.data(), static_cast<std::size_t>(n));
			} else if (n == 0 || errno != EINTR) {
				close_end(*read_ends.at(i));
				fds.at(i).fd = -1;
				--open_fds;
			}
		}
	}

	// Close what is left before waiting, a child that still writes would block
	close_end(out_pipe[0]);
	close_end(err_pipe[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (timed_out) {
		throw NoDeviceException(args.front() + " did not finish in time, is the camera delivering a picture?");
	}
	result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return result;
}

class FfmpegReader final : public FrameReader {
  public:
	FfmpegReader(std::string device_path, std::string device_format, int numframes = 10)
		: device_path_(std::move(device_path)), device_format_(std::move(device_format)), numframes_(numframes) {
	}

	bool set(int prop, double value) override {
		if (prop == cv::CAP_PROP_FRAME_WIDTH) {
			width_ = static_cast<int>(value);
			return true;
		}
		if (prop == cv::CAP_PROP_FRAME_HEIGHT) {
			height_ = static_cast<int>(value);
			return true;
		}
		return false;
	}

	[[nodiscard]] double get(int prop) const override {
		if (prop == cv::CAP_PROP_FRAME_WIDTH) {
			return width_;
		}
		if (prop == cv::CAP_PROP_FRAME_HEIGHT) {
			return height_;
		}
		return 0;
	}

	bool read(cv::Mat& frame) override {
		// The first call probes the camera and fills the frame buffer
		if (init_camera_) {
			init_camera_ = false;
			record();
			frame = frames_.at(0);
			return true;
		}

		if (frames_.empty() || num_frames_read_ >= numframes_ - 1) {
			record();
		}

		// Frame 0 is skipped on purpose, it is usually black as the IR emitter
		// is not fully active yet.
		++num_frames_read_;
		if (static_cast<std::size_t>(num_frames_read_) >= frames_.size()) {
			return false;
		}
		frame = frames_.at(static_cast<std::size_t>(num_frames_read_));
		return true;
	}

	void grab() override {
		cv::Mat ignored;
		read(ignored);
	}

	void release() override {
		frames_.clear();
		num_frames_read_ = 0;
	}

  private:
	void probe() {
		// ffmpeg exits with 1 here ("Immediate exit requested") by design
		const auto list = run_process({
			"ffmpeg",
			"-hide_banner",
			"-f",
			device_format_,
			"-list_formats",
			"all",
			"-i",
			device_path_,
		});

		static const std::regex RESOLUTION(R"(\s(\d{3,4})x(\d{3,4}))");
		std::smatch match;
		int width = 0;
		int height = 0;

		if (list.exit_code == 1 && std::regex_search(list.err, match, RESOLUTION)) {
			width = std::stoi(match[1].str());
			height = std::stoi(match[2].str());
		} else {
			const auto probe = run_process({
				"ffprobe",
				"-v",
				"error",
				"-select_streams",
				"v:0",
				"-show_entries",
				"stream=width,height",
				"-of",
				"csv=p=0:s=x",
				device_path_,
			});
			static const std::regex PROBE_RESOLUTION(R"((\d+)x(\d+))");
			if (std::regex_search(probe.out, match, PROBE_RESOLUTION)) {
				width = std::stoi(match[1].str());
				height = std::stoi(match[2].str());
			}
		}

		if (width <= 0 || height <= 0) {
			return;
		}
		// The picture is scaled to what was set, a size that was not set keeps its shape
		if (width_ == 0 && height_ == 0) {
			width_ = width;
			height_ = height;
		} else if (width_ == 0) {
			width_ = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * height_ / height)));
		} else if (height_ == 0) {
			height_ = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * width_ / width)));
		}
	}

	void record() {
		if (width_ == 0 || height_ == 0) {
			probe();
		}
		if (width_ <= 0 || height_ <= 0) {
			throw NoDeviceException(S("Could not determine the resolution of the camera through ffmpeg"));
		}

		num_frames_read_ = 0;

		// rgb24 is interpreted as BGR afterwards, exactly like the Python reader
		// does. Keep it that way so descriptors stay compatible with models that
		// were enrolled through the ffmpeg reader.
		const auto result = run_process({
			"ffmpeg",
			"-hide_banner",
			"-loglevel",
			"error",
			"-f",
			device_format_,
			"-i",
			device_path_,
			// The frames are cut by this size
			"-vf",
			"scale=" + std::to_string(width_) + ":" + std::to_string(height_),
			"-f",
			"rawvideo",
			"-pix_fmt",
			"rgb24",
			"-vframes",
			std::to_string(numframes_),
			"pipe:",
		});

		const std::size_t frame_size = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3;
		const std::size_t count = result.out.size() / frame_size;
		if (count == 0) {
			throw NoDeviceException(S("Failed to read camera specified in the 'device_path' config option, aborting"));
		}

		frames_.clear();
		for (std::size_t i = 0; i < count; ++i) {
			cv::Mat frame(height_, width_, CV_8UC3);
			std::memcpy(frame.data, result.out.data() + (i * frame_size), frame_size);
			frames_.push_back(frame);
		}
	}

	std::string device_path_;
	std::string device_format_;
	int numframes_;
	int width_ = 0;
	int height_ = 0;
	int num_frames_read_ = 0;
	bool init_camera_ = true;
	std::vector<cv::Mat> frames_;
};

} // namespace

// -----------------------------------------------------------------------------
// VideoCapture
// -----------------------------------------------------------------------------

VideoCapture::VideoCapture(const Config& config) : config_(config) {
	const std::string device_path = config_.get("video", "device_path");
	std::error_code ec;
	if (device_path.empty() || !std::filesystem::exists(device_path, ec)) {
		std::string reason = S("Howdy could not find a camera device at the path specified in the config file.\n");
		reason += S("It is very likely that the path is not configured correctly, please edit the 'device_path' config value by running:\n");
		reason += "\n\tsudo howdy config\n";
		throw NoDeviceException(reason);
	}

	exposure_ = config_.get_int("video", "exposure", -1);

	create_reader();

	// Request a frame to wake the camera up
	internal_->grab();
}

VideoCapture::~VideoCapture() {
	try {
		release();
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
}

void VideoCapture::release() {
	if (released_ || !internal_) {
		return;
	}
	released_ = true;
	if (original_brightness_.has_value()) {
		internal_->set(cv::CAP_PROP_BRIGHTNESS, *original_brightness_);
	}
	internal_->release();
}

std::pair<cv::Mat, cv::Mat> VideoCapture::read_frame() {
	if (exposure_ != -1) {
		// For a strange reason on some cameras (e.g. Lenovo X1E) setting manual
		// exposure works only after a couple frames are captured and even after a
		// delay it does not always work. Setting exposure at every frame is
		// reliable though. 1 = Manual
		internal_->set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
		internal_->set(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure_));
	}

	cv::Mat frame;
	if (!internal_->read(frame) || frame.empty()) {
		throw NoDeviceException(S("Failed to read camera specified in the 'device_path' config option, aborting"));
	}

	// Single channel frames come from grayscale or raw formats, see the
	// pixel_format option. Deeper than 8 bits gets scaled down first.
	if (frame.channels() == 1) {
		if (frame.depth() != CV_8U) {
			frame = scale_raw(frame);
		}
		cv::Mat color;
		cv::cvtColor(frame, color, cv::COLOR_GRAY2BGR);
		return {color, frame};
	}

	cv::Mat gray;
	if (frame.channels() == 4) {
		cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
		cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
	} else {
		cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
	}
	return {frame, gray};
}

cv::Mat VideoCapture::scale_raw(const cv::Mat& frame) const {
	// Scales a raw 10 to 16 bit grayscale frame down to 8 bits. The black level
	// is removed and the brightest 0.5% of pixels are clipped, so a dim IR image
	// still uses the full range.
	const auto black = static_cast<double>(config_.get_int("video", "raw_black_level", 0));

	cv::Mat f;
	frame.convertTo(f, CV_32F, 1.0, -black);
	cv::max(f, 0.0, f);

	// numpy.percentile(f, 99.5) with linear interpolation
	std::vector<float> values;
	values.assign(f.begin<float>(), f.end<float>());
	double top = 1.0;
	if (!values.empty()) {
		const double rank = 0.995 * static_cast<double>(values.size() - 1);
		const auto lower = static_cast<std::size_t>(rank);
		const double fraction = rank - static_cast<double>(lower);
		std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(lower), values.end());
		const double low_value = values[lower];
		double high_value = low_value;
		if (lower + 1 < values.size()) {
			high_value = *std::min_element(values.begin() + static_cast<std::ptrdiff_t>(lower + 1), values.end());
		}
		top = std::max(low_value + (fraction * (high_value - low_value)), 1.0);
	}

	cv::Mat result;
	// convertTo saturates to 0..255, which is the np.clip of the Python version
	f.convertTo(result, CV_8U, 255.0 / top);
	return result;
}

void VideoCapture::create_reader() {
	const std::string device_path = config_.get("video", "device_path");
	const std::string recording_plugin = config_.get("video", "recording_plugin", "opencv");

	if (recording_plugin == "ffmpeg") {
		internal_ = std::make_unique<FfmpegReader>(device_path, config_.get("video", "device_format", "v4l2"));
	} else {
		if (recording_plugin == "pyv4l2") {
			log(LOG_WARNING, "The pyv4l2 recording plugin is not available in the C++ daemon, using opencv instead");
		}

		// Start video capture on the IR camera through OpenCV
		internal_ = std::make_unique<OpenCVReader>(device_path);

		// Set the capture frame rate. Without this the first detected (and
		// possibly lower) frame rate is used, -1 seems to select the highest.
		// Use 0 as a fallback to avoid breaking an existing setup.
		const long fps = config_.get_int("video", "device_fps", 0);
		if (fps != 0) {
			internal_->set(cv::CAP_PROP_FPS, static_cast<double>(fps));
		}
	}

	const auto brightness = config_.get_optional_int("video", "brightness");
	if (brightness.has_value()) {
		original_brightness_ = internal_->get(cv::CAP_PROP_BRIGHTNESS);
		internal_->set(cv::CAP_PROP_BRIGHTNESS, static_cast<double>(*brightness));
	}

	// Force MJPEG decoding if true
	if (config_.get_bool("video", "force_mjpeg", false)) {
		internal_->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
	}

	// Request a specific pixel format, for IR sensors that only offer raw
	// formats OpenCV does not choose on its own (Y10 for example)
	std::string pixel_format = trim(config_.get("video", "pixel_format"));
	if (!pixel_format.empty()) {
		pixel_format.resize(4, ' ');
		internal_->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc(pixel_format[0], pixel_format[1], pixel_format[2], pixel_format[3]));
		// Keep the raw samples, read_frame() scales them down itself
		internal_->set(cv::CAP_PROP_CONVERT_RGB, 0);
	}

	// Set the frame width and height if requested
	const long fw = config_.get_int("video", "frame_width", -1);
	const long fh = config_.get_int("video", "frame_height", -1);
	if (fw != -1) {
		internal_->set(cv::CAP_PROP_FRAME_WIDTH, static_cast<double>(fw));
	}
	if (fh != -1) {
		internal_->set(cv::CAP_PROP_FRAME_HEIGHT, static_cast<double>(fh));
	}
}

} // namespace howdy

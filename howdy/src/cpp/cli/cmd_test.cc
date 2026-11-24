// howdy test: show a window with the camera and the detection results

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <tuple>

#include <opencv2/core/utils/logger.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "cli.hh"
#include "config.hh"
#include "face_backend.hh"
#include "liveness.hh"
#include "management_client.hh"
#include "paths.hh"
#include "rotation.hh"
#include "util.hh"
#include "video_capture.hh"

#ifdef HOWDY_WITH_OPENVINO
#include "openvino_backend.hh"
#endif

namespace howdy::cli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t COMPARE_FRAME_TIMEOUT_USEC = 10ULL * 1000000ULL;
// How long a camera that is there but busy is waited for, and how often it is tried
constexpr std::chrono::seconds CAMERA_WAIT{5};
constexpr std::chrono::milliseconds CAMERA_RETRY_INTERVAL{250};
// The shortest time between two rounds of comparisons
constexpr std::chrono::milliseconds COMPARE_INTERVAL{100};

constexpr const char* WINDOW_NAME = "Howdy Test";
// The video of the window is shown this much larger than the frames of the camera
constexpr double DISPLAY_SCALE = 1.5;

// The overlay (text, bars, ellipses) is drawn on the enlarged video, not on the frame of the camera
// before it is enlarged: drawn small and then stretched, the text is blurry. The positions and
// sizes below are the ones of the frame, this turns them into pixels of the window.
int px(double value) {
	return static_cast<int>(std::lround(value * DISPLAY_SCALE));
}

cv::Point px(cv::Point point) {
	return {px(point.x), px(point.y)};
}

constexpr double FONT_SCALE_SMALL = 0.3 * DISPLAY_SCALE;
constexpr double FONT_SCALE_NORMAL = 0.5 * DISPLAY_SCALE;

double certainty_to_probability(double certainty) {
	const double c = std::max(0.0, std::min(1.0, certainty));
	if (c >= 0.5) {
		return (1.0 - c) * 100.0;
	}
	return 100.0 - (50.0 * std::pow(2.0 * c, 1.0 / 0.569323));
}

std::string fixed(double value, int digits) {
	std::array<char, 64> buffer{};
	std::snprintf(buffer.data(), buffer.size(), "%.*f", digits, value);
	return buffer.data();
}

bool g_slow_mode = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): toggled by the mouse callback

void on_mouse(int event, int /*x*/, int /*y*/, int /*flags*/, void* /*userdata*/) {
	// Toggle slow mode on click
	if (event == cv::EVENT_LBUTTONDOWN) {
		g_slow_mode = !g_slow_mode;
	}
}

// Print the status text by line number
void print_text(cv::Mat& overlay, int line_number, const std::string& text) {
	cv::putText(
		overlay, text, cv::Point(px(10), overlay.rows - px(10) - px(10 * line_number)), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_SMALL, cv::Scalar(0, 255, 0), 1,
		cv::LINE_AA);
}

// Compares faces with the models through the management daemon, in a thread of its own: a call
// takes a few milliseconds and more, and a frame must not wait for it (the camera would deliver
// the same kind of frame over and over, see the IR emitter). The newest face of every position in
// the frame is kept, the window draws the last answer. The bus is only used by the thread after
// the constructor.
class CompareWorker {
  public:
	enum class State : std::uint8_t { Running, NoModels, Failed };

	struct Update {
		State state = State::Running;
		std::string error;
		// The last answer for every position in the frame
		std::map<std::size_t, FaceMatch> matches;
	};

	CompareWorker(BusPtr bus, std::string user, std::string metric) : bus_(std::move(bus)), user_(std::move(user)), metric_(std::move(metric)) {
		thread_ = std::thread([this] { run(); });
	}

	~CompareWorker() {
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		wake_.notify_one();
		thread_.join();
	}

	CompareWorker(const CompareWorker&) = delete;
	CompareWorker& operator=(const CompareWorker&) = delete;
	CompareWorker(CompareWorker&&) = delete;
	CompareWorker& operator=(CompareWorker&&) = delete;

	// Compare the face at this position of the frame, replaces one that did not start yet
	void submit(std::size_t slot, Encoding encoding) {
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			pending_[slot] = std::move(encoding);
		}
		wake_.notify_one();
	}

	Update update() {
		const std::lock_guard<std::mutex> lock(mutex_);
		return update_;
	}

  private:
	void run() {
		auto next_allowed = Clock::now();
		for (;;) {
			std::map<std::size_t, Encoding> work;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				wake_.wait(lock, [this] { return stop_.load() || !pending_.empty(); });
				if (stop_) {
					return;
				}
				// Not more often than every COMPARE_INTERVAL: the label is up to date ten times a second,
				// and the daemon and the bus do not take the time the frames need. Faces that come in
				// meanwhile replace the waiting ones.
				if (wake_.wait_until(lock, next_allowed, [this] { return stop_.load(); })) {
					return;
				}
				work.swap(pending_);
			}
			next_allowed = Clock::now() + COMPARE_INTERVAL;

			for (auto& [slot, encoding] : work) {
				std::optional<FaceMatch> found;
				try {
					found = compare_face(bus_.get(), user_, metric_, encoding, COMPARE_FRAME_TIMEOUT_USEC, [this] { return stop_.load(); });
				} catch (const CallCancelled&) {
					return;
				} catch (const std::exception& exc) {
					const std::lock_guard<std::mutex> lock(mutex_);
					update_.state = State::Failed;
					update_.error = exc.what();
					return;
				}

				const std::lock_guard<std::mutex> lock(mutex_);
				if (found.has_value()) {
					update_.matches[slot] = std::move(*found);
				} else {
					update_.state = State::NoModels;
					return;
				}
			}
		}
	}

	BusPtr bus_;
	std::string user_;
	std::string metric_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::map<std::size_t, Encoding> pending_;
	Update update_;
	// Set under the mutex (the condition variable needs that), read without by the calls that wait
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

// Everything the test loop needs besides the camera
struct TestContext {
	std::unique_ptr<FaceBackend> backend;
#ifdef HOWDY_WITH_OPENVINO
	std::unique_ptr<SpoofDetector> spoof_detector;
	std::unique_ptr<SpoofDetectorFlir> spoof_detector_flir;
#endif
	bool use_openvino = false;
	double video_certainty = 0;
	double dark_threshold = 0;
	double identical_threshold = 0;

	// The matching works like it does in the authentication
	MicroMotionTracker micromotion_detector;
	TextureDetector texture_detector;
	SimilarityDetector similarity_detector;

	// The models are not read here, the management daemon compares the faces with them
	BusPtr bus;
	std::unique_ptr<CompareWorker> compare;
	// Off if the user has no model or the daemon can not be reached
	bool compare_enabled = true;
};

// The best match of a face encoding with the stored models
struct Match {
	double value = 0;
	std::string label;
	double certainty = 0;
	bool accept = false;
};

// Open the camera. A face authentication (pam_howdy) may have used it just before, and the camera is
// busy for a moment after that: the driver needs its time to stop the stream and to
// let go of the device. So a camera that is there but can not be read is tried again for a while
// (reading one frame shows that), instead of a pause that is a guess and costs time when the camera is
// free. A device path that does not exist fails at once.
std::unique_ptr<VideoCapture> open_camera(const Config& config) {
	const std::string device_path = config.get("video", "device_path", "none");
	const auto exists = [&] { return device_path == "none" || std::filesystem::exists(device_path); };

	// The warnings of OpenCV and GStreamer come with every try
	const auto log_level = cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
	const auto restore_log_level = ScopeExit([&] { cv::utils::logging::setLogLevel(log_level); });

	const auto deadline = Clock::now() + CAMERA_WAIT;
	bool announced = false;
	for (;;) {
		try {
			auto capture = std::make_unique<VideoCapture>(config);
			// A camera that is opened but still busy fails on the first read
			static_cast<void>(capture->read_frame());
			return capture;
		} catch (const NoDeviceException& exc) {
			if (!exists() || Clock::now() >= deadline) {
				throw CliError(exc.what(), EXIT_INVALID_DEVICE);
			}
			if (!announced) {
				announced = true;
				std::cout << "Waiting for the camera ...\n";
			}
			std::this_thread::sleep_for(CAMERA_RETRY_INTERVAL);
		}
	}
}

// Create the OpenVINO backend and its detectors. Like the authentication this does
// not fall back to dlib, if the models are missing they have to be installed.
void setup_openvino([[maybe_unused]] const Config& config, [[maybe_unused]] const Options& options, [[maybe_unused]] TestContext& context) {
#ifdef HOWDY_WITH_OPENVINO
	const std::string device = to_upper(config.get("video", "openvino_device", "GPU"));
	try {
		// This runs as the user, who can not write to the system wide cache
		const auto home = lookup_home(options.user);
		if (home.has_value()) {
			const std::filesystem::path cache = std::filesystem::path(*home) / ".cache" / "howdy" / "openvino";
			std::error_code ec;
			std::filesystem::create_directories(cache, ec);
			if (!ec) {
				set_openvino_cache_dir(cache.string());
			}
		}
		context.backend = create_openvino_backend(device, true);
		context.spoof_detector = std::make_unique<SpoofDetector>(device);
		context.spoof_detector_flir = std::make_unique<SpoofDetectorFlir>(device);
		std::cout << "Using OpenVINO " << device << " for face detection and encoding" << '\n';
	} catch (const ModelException& exc) {
		// The message is printed to the console by main()
		throw CliError(
			std::string("OpenVINO init failed: '") + exc.what() + "'\n" +
			S("OpenVINO data files have not been downloaded, please run the following commands:") + "\n\tcd " + paths::OPENVINO_DATA_DIR +
			"\n\tsudo ./install.sh");
	}
#else
	throw CliError("use_openvino is enabled, but howdy was built without OpenVINO support");
#endif
}

// Choose and create the face backend
void setup_backend(const Config& config, const Options& options, TestContext& context) {
	context.use_openvino = config.get_bool("face_authentication", "use_openvino", false);

	if (context.use_openvino) {
		setup_openvino(config, options, context);
		context.video_certainty = 1 - context.video_certainty;
	} else {
		try {
			context.backend = create_dlib_backend(config.get_bool("face_authentication", "use_cnn", false));
		} catch (const ModelException& exc) {
			throw CliError(exc.what());
		}
	}
}

void setup_detectors(const Config& config, TestContext& context) {
	context.micromotion_detector.init(static_cast<int>(config.get_int("face_authentication", "micromotion_detection_buffer_size", 8)));
	context.texture_detector.init(static_cast<int>(config.get_int("face_authentication", "texture_detection_buffer_size", 5)));
	context.similarity_detector.init(static_cast<int>(config.get_int("face_authentication", "similarity_detection_buffer_size", 8)));
}

// Counts the frames and the frames per second
class FrameCounter {
  public:
	void tick(Clock::time_point frame_start) {
		++total_frames_;
		++sec_frames_;

		// If we've entered a new second
		const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(frame_start.time_since_epoch()).count();
		if (sec_ != now_sec) {
			// Set the last seconds FPS, the new second and reset the counter
			fps_ = sec_frames_;
			sec_ = now_sec;
			sec_frames_ = 0;
		}
	}

	[[nodiscard]] int total_frames() const {
		return total_frames_;
	}
	[[nodiscard]] int fps() const {
		return fps_;
	}

  private:
	// Count all frames ever
	int total_frames_ = 0;
	// Count all frames per second
	int sec_frames_ = 0;
	// Last seconds FPS
	int fps_ = 0;
	// The current second we're counting
	std::chrono::seconds::rep sec_ = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
};

// Grab a single frame of video, rotated and equalized
void read_frames(VideoCapture& video_capture, int rotate, int frame_number, const cv::Ptr<cv::CLAHE>& clahe, cv::Mat& frame, cv::Mat& gsframe) {
	try {
		std::tie(frame, gsframe) = video_capture.read_frame();
	} catch (const NoDeviceException& exc) {
		throw CliError(exc.what(), EXIT_INVALID_DEVICE);
	}
	rotate_frame(frame, rotate, frame_number);
	rotate_frame(gsframe, rotate, frame_number);
	clahe->apply(gsframe, gsframe);
}

// Create a histogram of the image with 8 values, draw the bars in the top left
// of the overlay and return the percentages
std::array<double, 8> draw_histogram(const cv::Mat& gsframe, cv::Mat& overlay) {
	cv::Mat hist;
	const int channels = 0;
	const int hist_size = 8;
	const std::array<float, 2> range{0, 256};
	const float* ranges = range.data();
	cv::calcHist(&gsframe, 1, &channels, cv::Mat(), hist, 1, &hist_size, &ranges);
	const double hist_total = cv::sum(hist)[0];

	// Fill with the overall containing percentage
	std::array<double, 8> hist_perc{};

	// Loop though all values to calculate a percentage and add it to the overlay
	for (int index = 0; index < hist_size; ++index) {
		const double value_perc = static_cast<double>(hist.at<float>(index)) / hist_total * 100;
		hist_perc.at(static_cast<std::size_t>(index)) = value_perc;

		// Top left point, 10px margins, and a bottom right point that makes the
		// bar 10px thick, with a height of half the percentage
		cv::rectangle(
			overlay, px(cv::Point(20 + (10 * index), 10)), px(cv::Point(10 + (10 * index), static_cast<int>((value_perc / 2) + 10))), cv::Scalar(0, 200, 0),
			cv::FILLED);
	}
	return hist_perc;
}

// Print the status in the bottom left
void draw_status(cv::Mat& overlay, const cv::Size& frame_size, const FrameCounter& counter, double rec_tm) {
	print_text(overlay, 0, "RESOLUTION: " + std::to_string(frame_size.height) + "x" + std::to_string(frame_size.width));
	print_text(overlay, 1, "FPS: " + std::to_string(counter.fps()));
	print_text(overlay, 2, "FRAMES: " + std::to_string(counter.total_frames()));
	print_text(overlay, 3, "RECOGNITION: " + std::to_string(static_cast<int>(std::lround(rec_tm * 1000))) + "ms");

	// Show that slow mode is on, if it's on
	if (g_slow_mode) {
		cv::putText(
			overlay, "SLOW MODE", cv::Point(overlay.cols - px(66), overlay.rows - px(10)), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_SMALL, cv::Scalar(0, 0, 255), 1,
			cv::LINE_AA);
	}
}

// The best match of the face at this position of the frame, as the management daemon answered it
// for the last frame, std::nullopt if there is none yet. If the user has no model or the daemon can
// not be asked, the comparison is switched off.
std::optional<Match> find_match(TestContext& context, std::size_t slot, const Encoding& face_encoding) {
	context.compare->submit(slot, face_encoding);

	const auto update = context.compare->update();
	if (update.state == CompareWorker::State::Failed) {
		std::cout << "Could not compare the face with the face models (" << update.error << "), no matches will be shown\n" << '\n';
		context.compare_enabled = false;
		return std::nullopt;
	}
	if (update.state == CompareWorker::State::NoModels) {
		context.compare_enabled = false;
		return std::nullopt;
	}

	const auto found = update.matches.find(slot);
	if (found == update.matches.end()) {
		return std::nullopt;
	}

	Match result;
	result.value = found->second.value;
	result.label = found->second.label;
	if (context.use_openvino) {
		result.certainty = certainty_to_probability(1.0 - result.value);
		result.accept = context.video_certainty < result.value && result.value < 1;
	} else {
		result.certainty = certainty_to_probability(result.value);
		result.accept = context.video_certainty > result.value && result.value > 0;
	}
	return result;
}

// Print the values of the spoof, motion and texture detection of a face
void print_liveness(TestContext& context, const cv::Mat& frame, const cv::Mat& gsframe, const dlib::rectangle& loc) {
#ifdef HOWDY_WITH_OPENVINO
	if (context.spoof_detector_flir) {
		std::cout << "damo-flir: " << fixed((*context.spoof_detector_flir)(frame, loc) * 100, 3) << "%" << '\n';
	}
#endif

	std::cout << "Micro-movement: " << fixed(context.micromotion_detector.calculate_displacement(context.backend->motion_landmarks(frame, loc)), 4) << '\n';

	const auto texture = context.texture_detector(gsframe, loc);
	if (!texture.has_value()) {
		std::cout << "No texture" << '\n';
	} else {
		std::cout << "texture: " << fixed(*texture, 3) << '\n';
	}
}

// Compare the face with the models of the user and print the name next to it.
// Returns false if the face can not be encoded, the color is green for a match.
bool label_face(
	TestContext& context, std::size_t index, const cv::Mat& frame, const dlib::rectangle& loc, cv::Point center, cv::Size radius, cv::Mat& overlay,
	cv::Scalar& color) {
	const auto face_encoding = context.backend->encode(frame, loc);
	if (!face_encoding.has_value()) {
		return false;
	}

	const auto [similarity, stale] = context.similarity_detector(*face_encoding, context.identical_threshold);
	std::cout << "similarity: " << fixed(similarity * 100, 3) << '\n';
	std::cout << "stale: " << (stale ? "True" : "False") << '\n';

	const auto found = find_match(context, index, *face_encoding);
	if (!found.has_value()) {
		return true;
	}
	const Match& match = *found;

	const cv::Point text_position(static_cast<int>(center.x + (radius.width / 3.0)), center.y - radius.height);
	if (match.accept) {
		// Turn the ellipse green and print the name of the model next to it
		color = cv::Scalar(0, 230, 0);
		cv::putText(
			overlay, match.label + " (" + fixed(match.certainty, 3) + ")", px(text_position), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_NORMAL, color, 1,
			cv::LINE_AA);
	} else {
		// If no approved matches, show red text
		cv::putText(
			overlay, "no match (" + fixed(match.certainty, 3) + ")", px(text_position), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_NORMAL, color, 1, cv::LINE_AA);
	}
	return true;
}

// Paint an ellipse around a face, with the match result if the user has models
void draw_face(TestContext& context, std::size_t index, const cv::Mat& frame, const cv::Mat& gsframe, const dlib::rectangle& loc, cv::Mat& overlay) {
	print_liveness(context, frame, gsframe, loc);

	// By default the ellipse around the face is red for no match
	cv::Scalar color(0, 0, 230); // BGR

	const double half_width = static_cast<double>(loc.right() - loc.left()) / 2;
	const double half_height = static_cast<double>(loc.bottom() - loc.top()) / 2;

	// Get the center X and Y from the rectangular points
	const cv::Point center(
		gsframe.cols - (static_cast<int>(half_width) + static_cast<int>(loc.left())), static_cast<int>(half_height) + static_cast<int>(loc.top()));

	// Get the radius from the with of the square
	const cv::Size radius(static_cast<int>(half_width * 1.2), static_cast<int>(half_height * 1.2));

	// If we have models defined for the current user
	if (context.compare_enabled) {
		if (!label_face(context, index, frame, loc, center, radius, overlay, color)) {
			return;
		}
	}

	// Draw the ellipse
	cv::ellipse(overlay, px(center), cv::Size(px(radius.width), px(radius.height)), 0, 0, 360, color, px(3));
}

// Detect the faces of an active frame and show the results, returns the recognition time
double scan_frame(TestContext& context, const cv::Mat& frame, const cv::Mat& gsframe, cv::Mat& overlay) {
	// Show that this is an active frame
	cv::putText(overlay, "SCAN FRAME", px(cv::Point(gsframe.cols - 68, 16)), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_SMALL, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

	const auto recognition_start = Clock::now();
	// Get the locations of all faces, upsampled once
	const auto face_locations = context.backend->detect(gsframe);
	const double recognition_time = std::chrono::duration<double>(Clock::now() - recognition_start).count();

#ifdef HOWDY_WITH_OPENVINO
	if (context.spoof_detector) {
		std::cout << "spoof: " << fixed((*context.spoof_detector)(frame) * 100, 3) << "%" << '\n';
	}
#endif

	// Loop though all faces and paint an ellipse around them
	for (std::size_t index = 0; index < face_locations.size(); ++index) {
		draw_face(context, index, frame, gsframe, face_locations[index], overlay);
	}

	std::cout << "-------------------------------------------------------" << '\n';
	std::cout.flush();
	return recognition_time;
}

// Quit on q or if the window has been closed, imshow would otherwise open a new one
bool quit_requested() {
	if ((cv::waitKey(1) & 0xFF) == 'q') {
		return true;
	}

	// The Qt backend throws an error instead of returning -1 for a closed window
	try {
		return cv::getWindowProperty(WINDOW_NAME, cv::WND_PROP_VISIBLE) < 1;
	} catch (const cv::Exception&) {
		return true;
	}
}

} // namespace

int run_test(const Options& options, const std::vector<std::string>& original_arguments) {
	// The window of OpenCV (Qt) is slow on a native Wayland window, the video stutters, and smooth
	// when it runs through XWayland. Like the other windows of Howdy (GDK_BACKEND) it always uses X.
	setenv("QT_QPA_PLATFORM", "xcb", 1);

	// The daemon only compares faces for root: a program that may ask often enough could work out the
	// models from the answers. Root needs no polkit for it.
	require_root(original_arguments);
	BusPtr bus = open_system_bus();

	// Read config from disk
	const Config config((std::filesystem::path(paths::CONFIG_DIR) / "config.ini").string());

	const std::unique_ptr<VideoCapture> video_capture = open_camera(config);

	// Read config values to use in the main loop
	TestContext context;
	context.bus = std::move(bus);
	context.video_certainty = std::round(probability_to_certainty(config.get_float("face_authentication", "certainty", 75)) * 100000) / 100000;
	context.dark_threshold = config.get_float("video", "dark_threshold", 60);
	context.identical_threshold = config.get_float("face_authentication", "identical_threshold", 99.75) / 100;
	// Only the fixed rotation modes, the alternating ones would flicker
	const int rotate = fixed_rotation(static_cast<int>(config.get_int("video", "rotate", 0)));

	// Let the user know what's up
	std::cout << "\n"
			  << S("Opening a window with a test feed\n\nPress ctrl+C in this terminal to quit\nClick on the image to enable or disable slow mode\n") << '\n';

	setup_backend(config, options, context);
	setup_detectors(config, context);
	// The bus is the thread's from here on
	context.compare = std::make_unique<CompareWorker>(std::move(context.bus), options.user, context.use_openvino ? "dot" : "euclidean");

	const auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));

	// Open the window and attach a mouse listener
	cv::namedWindow(WINDOW_NAME);
	cv::setMouseCallback(WINDOW_NAME, on_mouse);

	FrameCounter counter;
	// Recognition time
	double rec_tm = 0;

	for (;;) {
		const auto frame_start = Clock::now();
		counter.tick(frame_start);

		cv::Mat frame;
		cv::Mat gsframe;
		read_frames(*video_capture, rotate, counter.total_frames(), clahe, frame, gsframe);

		// The video as it is shown, enlarged, and a copy of it to draw the overlays in
		cv::Mat shown;
		cv::cvtColor(gsframe, shown, cv::COLOR_GRAY2BGR);
		cv::flip(shown, shown, 1);
		cv::resize(shown, shown, cv::Size(), DISPLAY_SCALE, DISPLAY_SCALE, cv::INTER_CUBIC);
		cv::Mat overlay = shown.clone();

		const auto hist_perc = draw_histogram(gsframe, overlay);
		draw_status(overlay, gsframe.size(), counter, rec_tm);

		// Ignore dark frames
		if (hist_perc[0] > context.dark_threshold) {
			// Show that this is an ignored frame in the top right
			cv::putText(
				overlay, "DARK FRAME", px(cv::Point(gsframe.cols - 68, 16)), cv::FONT_HERSHEY_SIMPLEX, FONT_SCALE_SMALL, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
		} else {
			rec_tm = scan_frame(context, frame, gsframe, overlay);
		}

		// Add the overlay to the frame with some transparency
		const double alpha = 0.65;
		cv::addWeighted(overlay, alpha, shown, 1 - alpha, 0, shown);

		cv::imshow(WINDOW_NAME, shown);

		if (quit_requested()) {
			break;
		}

		// Delay the frame if slowmode is on
		if (g_slow_mode) {
			const double frame_time = std::chrono::duration<double>(Clock::now() - frame_start).count();
			std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.5 - frame_time, 0.0)));
		}
	}

	// Let the user know we're stopping and release the window
	std::cout << S("\nClosing window") << '\n';
	cv::destroyAllWindows();
	return 0;
}

} // namespace howdy::cli

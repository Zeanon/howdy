#include "authenticator.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <unistd.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "paths.hh"
#include "rotation.hh"
#include "rubberstamps/rubberstamps.hh"
#include "snapshot.hh"
#include "user_models.hh"
#include "util.hh"

namespace howdy {

const char* to_string(Status status) noexcept {
	switch (status) {
		case Status::Running: return "RUNNING";
		case Status::Success: return "SUCCESS";
		case Status::NoMatch: return "NO_MATCH";
		case Status::NoEnrolledFaces: return "NO_ENROLLED_FACE";
		case Status::Timeout: return "TIMEOUT";
		case Status::TooDark: return "TOO_DARK";
		case Status::Cancelled: return "CANCELLED";
		case Status::Cancelling: return "CANCELLING";
		case Status::Error: return "ERROR";
		case Status::NotInitialized: return "NOT_INITIALIZED";
		case Status::NoDevice: return "NO_DEVICE";
		case Status::KeepAliveTimeout: return "KEEPALIVE_TIMEOUT";
	}
	return "ERROR";
}

namespace {

// The longest timeout a caller without privileges can ask for, if the config does not allow more
constexpr double UNPRIVILEGED_MAX_TIMEOUT = 60;

bool starts_with(const std::string& value, const char* prefix, std::string& rest) {
	const std::string_view view(prefix);
	if (!value.starts_with(view)) {
		return false;
	}
	rest = value.substr(view.size());
	return true;
}

double round_to(double value, int digits) {
	const double factor = std::pow(10.0, digits);
	return std::nearbyint(value * factor) / factor;
}

// Save the first frames of the scan with some metadata, see [snapshots] in config.ini.
// A snapshot that can't be saved must never change the result of the authentication.
void make_snapshot(
	const std::vector<cv::Mat>& frames, const std::string& type, double scan_time, long frame_count, double lowest_certainty,
	const SnapshotRetention& retention) {
	try {
		const std::time_t now = std::time(nullptr);
		std::tm utc{};
		gmtime_r(&now, &utc);
		std::array<char, 32> date{};
		std::strftime(date.data(), date.size(), "%Y/%m/%d %H:%M:%S UTC", &utc);

		std::array<char, 256> hostname{};
		gethostname(hostname.data(), hostname.size() - 1);

		generate_snapshot(
			frames, {
						type + S(" LOGIN"),
						std::string(S("Date: ")) + date.data(),
						std::string(S("Scan time: ")) + format_double(round_to(scan_time, 2)) + "s",
						std::string(S("Frames: ")) + std::to_string(frame_count) + " (" +
							format_double(round_to(static_cast<double>(frame_count) / std::max(scan_time, 1e-9), 2)) + "FPS)",
						std::string(S("Hostname: ")) + hostname.data(),
						std::string(S("Best certainty value: ")) + format_double(round_to(lowest_certainty * 10, 1)),
					});
		prune_snapshots(retention);
	} catch (const std::exception& exc) {
		log(LOG_ERR, std::string("Could not save the snapshot: ") + exc.what());
	}
}

// std::nullopt if the file can not be read
std::optional<std::string> file_text(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

Authenticator::Authenticator(PopupSink& popup)
	: popup_(popup), config_path_((std::filesystem::path(paths::CONFIG_DIR) / "config.ini").string()),
	  // Read before it is parsed, so that a change in between is seen at the next request
	  config_text_(file_text(config_path_)), config_(config_path_) {
	load_settings();
}

void Authenticator::load_settings() {
	if (config_.parse_error() != 0) {
		log(LOG_WARNING, "Could not parse the config file completely, error at line " + std::to_string(config_.parse_error()));
	}
	read_config();
	// Old snapshots go away even if no new ones are saved any more
	try {
		prune_snapshots(snapshot_retention_);
	} catch (const std::exception& exc) {
		log(LOG_WARNING, std::string("Could not delete the old snapshots: ") + exc.what());
	}

	// The nod rubber stamp needs the landmarks too
	const bool landmarks = use_micromotion_detection_ || config_.get_bool("rubberstamps", "enabled", false);
	const ModelOptions options{use_openvino_, use_cnn_, openvino_device_, use_anti_spoof_, use_damo_flir_, landmarks};
	if (model_options_ != options) {
		model_options_.reset();
		initialize_models(landmarks);
		model_options_ = options;
	}

	similarity_detector_.reset();
	if (use_similarity_detection_) {
		similarity_detector_.emplace();
	}
}

bool Authenticator::load_if_needed(bool changed) {
	// Without models the last try has failed
	if (backend_ && !changed) {
		return true;
	}
	const std::lock_guard<std::mutex> lock(models_mutex_);
	auto text = file_text(config_path_);
	// A file that can not be read right now, while an editor replaces it, keeps the settings
	if (backend_ && !text.has_value()) {
		return true;
	}

	log(LOG_INFO, "Loading the changed config");
	try {
		config_ = Config(config_path_);
		load_settings();
		config_text_ = std::move(text);
	} catch (const std::exception& exc) {
		log(LOG_ERR, std::string("Could not load the changed config: ") + exc.what());
		// Nothing is authenticated with half of it, the next request tries again
		backend_.reset();
		model_options_.reset();
		return false;
	}
	return true;
}

void Authenticator::read_config() {
	const std::string fa = "face_authentication";

	use_cnn_ = config_.get_bool(fa, "use_cnn", false);
	use_openvino_ = config_.get_bool(fa, "use_openvino", false);
	timeout_ = static_cast<double>(str_to_seconds(config_.get(fa, "timeout", "4s")));
	start_cooldown_ = std::chrono::seconds(std::max(0LL, str_to_seconds(config_.get(fa, "start_cooldown", "2s"))));
	const double certainty = config_.get_float(fa, "certainty", 75);
	// The setup of an older version has written values of another scale (2.2 to 4.2)
	if (certainty < 50) {
		log(LOG_WARNING, "The certainty of " + format_double(certainty) +
							 " in [face_authentication] is very low, faces of other people may be accepted. It is a percentage, the default is 75");
	}
	certainty_ = round_to(probability_to_certainty(certainty), 5);
	// Invert certainty if we use openvino as for openvino 1 equals full match,
	// while for dlib 0 equals full match
	if (use_openvino_) {
		certainty_ = 1 - certainty_;
	}
	confirmations_ = std::max(1, static_cast<int>(config_.get_int(fa, "confirmations", 5)));

	use_anti_spoof_ = config_.get_bool(fa, "use_anti_spoof", true);
	anti_spoof_threshold_ = 1 - (config_.get_float(fa, "anti_spoof_threshold", 95) / 100);
	use_damo_flir_ = config_.get_bool(fa, "use_damo_flir", true);
	damo_flir_threshold_ = config_.get_float(fa, "damo_flir_threshold", 95) / 100;

	use_micromotion_detection_ = config_.get_bool(fa, "micromotion_detection", true);
	micromotion_detection_buffer_size_ = static_cast<int>(config_.get_int(fa, "micromotion_detection_buffer_size", 8));
	micromotion_threshold_ = config_.get_float(fa, "micromotion_threshold", 0.03);

	use_texture_detection_ = config_.get_bool(fa, "texture_detection", true);
	texture_detection_buffer_size_ = static_cast<int>(config_.get_int(fa, "texture_detection_buffer_size", 5));
	texture_threshold_ = config_.get_float(fa, "texture_threshold", 35.0);

	use_similarity_detection_ = config_.get_bool(fa, "similarity_detection", true);
	similarity_detection_buffer_size_ = static_cast<int>(config_.get_int(fa, "similarity_detection_buffer_size", 8));
	identical_threshold_ = config_.get_float(fa, "identical_threshold", 99.75) / 100;
	sudden_change_threshold_ = config_.get_float(fa, "sudden_change_threshold", 90.0) / 100;

	auth_popup_ = config_.get_bool(fa, "show_auth_popup", false);
	save_failed_ = config_.get_bool("snapshots", "save_failed", false);
	save_successful_ = config_.get_bool("snapshots", "save_successful", false);
	snapshot_retention_.max_age = std::chrono::seconds(std::max(0LL, str_to_seconds(config_.get("snapshots", "retention", "7d"))));
	snapshot_retention_.max_count = static_cast<std::size_t>(std::max(0L, config_.get_int("snapshots", "max_snapshots", 100)));
	dark_threshold_ = config_.get_float("video", "dark_threshold", 50.0);
	max_height_ = config_.get_float("video", "max_height", 320.0);
	rotate_ = static_cast<int>(config_.get_int("video", "rotate", 0));
	openvino_device_ = trim(to_upper(config_.get("video", "openvino_device", "GPU")), "\"");
}

void Authenticator::initialize_models(bool landmarks) {
	// The old ones go first, both at once could be too much for a GPU
	backend_.reset();
#ifdef HOWDY_WITH_OPENVINO
	spoof_detector_.reset();
	spoof_detector_flir_.reset();
#else
	static_cast<void>(landmarks);
#endif

	if (use_openvino_) {
#ifdef HOWDY_WITH_OPENVINO
		try {
			backend_ = create_openvino_backend(openvino_device_, landmarks);
			if (use_anti_spoof_) {
				spoof_detector_ = std::make_unique<SpoofDetector>(openvino_device_);
			}
			if (use_damo_flir_) {
				spoof_detector_flir_ = std::make_unique<SpoofDetectorFlir>(openvino_device_);
			}
		} catch (const ModelException& exc) {
			log(LOG_ERR, std::string("OpenVINO init failed: '") + exc.what() + "'");
			log(LOG_ERR, S("OpenVINO data files have not been downloaded, please run the following commands:"));
			log(LOG_ERR, std::string("\tcd ") + paths::OPENVINO_DATA_DIR);
			log(LOG_ERR, "\tsudo ./install.sh");
			throw;
		} catch (const std::exception& exc) {
			log(LOG_ERR, std::string("OpenVINO init failed: ") + exc.what());
			throw;
		}
#else
		throw std::runtime_error("use_openvino is enabled, but howdy was built without OpenVINO support");
#endif
	} else {
		try {
			backend_ = create_dlib_backend(use_cnn_);
		} catch (const ModelException& exc) {
			log(LOG_ERR, exc.what());
			log(LOG_ERR, S("DLIB data files have not been downloaded, please run the following commands:"));
			log(LOG_ERR, std::string("\tcd ") + paths::DLIB_DATA_DIR);
			log(LOG_ERR, "\tsudo ./install.sh");
			throw;
		}
	}
}

Authenticator::Args Authenticator::parse_args(const std::vector<std::string>& args, bool limit_timeout) const {
	Args result{auth_popup_, timeout_};

	std::string value;
	for (const auto& arg : args) {
		if (arg == "show-auth-popup") {
			result.auth_popup = true;
		} else if (starts_with(arg, "timeout=", value)) {
			result.timeout = static_cast<double>(str_to_seconds(value));
		} else if (starts_with(arg, "show-auth-popup=", value)) {
			result.auth_popup = str_to_bool(value);
		} else {
			// The thresholds and the other settings come from the config file only
			log(LOG_WARNING, "Ignoring the unknown argument '" + arg + "'");
		}
	}

	// A caller without privileges can not keep the camera for longer than the config says (or a minute,
	// for a PAM configuration that asks for more than that): others get Busy meanwhile
	if (limit_timeout && timeout_ >= 1) {
		const double longest = std::max(timeout_, UNPRIVILEGED_MAX_TIMEOUT);
		if (result.timeout < 1 || result.timeout > longest) {
			log(LOG_WARNING, "The requested timeout is too long for this caller, using " + format_double(longest) + "s");
			result.timeout = longest;
		}
	}

	// A timeout below 1 means no timeout
	if (result.timeout < 1) {
		result.timeout = std::numeric_limits<double>::infinity();
	}
	return result;
}

Status Authenticator::authenticate(const std::string& username, const std::vector<std::string>& args, const RequestControl& control) {
	// Only one instance can use the camera at the same time.
	const std::lock_guard<std::mutex> lock(camera_lock_);
	if (control.cancel.load()) {
		return Status::Cancelled;
	}
	// Whatever changed the config, howdy set or an editor, it counts from now on
	if (!load_if_needed(control.config_changed)) {
		return Status::NotInitialized;
	}

	// Release the camera and close the popup, however this function is left
	const ScopeExit cleanup_on_exit([this] { cleanup(); });

	Status status = Status::Error;
	try {
		const Args parsed = parse_args(args, control.limit_timeout);

		if (similarity_detector_) {
			similarity_detector_->init(similarity_detection_buffer_size_);
		}

		if (parsed.auth_popup) {
			ui_uid_ = lookup_uid(username);
			if (ui_uid_.has_value()) {
				// popup_.show(*ui_uid_, S("Starting up..."));
				popup_.show(*ui_uid_, S("Face authentication..."));
			} else {
				log(LOG_ERR, "Howdy authentication popup could not be created");
			}
		}

		try {
			video_capture_ = std::make_unique<VideoCapture>(config_);
		} catch (const NoDeviceException& exc) {
			log(LOG_ERR, exc.what());
			return Status::NoDevice;
		}

		status = run(username, parsed, control);
	} catch (const std::exception& exc) {
		log(LOG_ERR, std::string("Howdy authentication failed with exception: ") + exc.what());
		status = Status::Error;
	} catch (...) {
		log(LOG_ERR, "Howdy authentication failed with an unknown exception");
		status = Status::Error;
	}

	return status;
}

void Authenticator::cleanup() {
	if (similarity_detector_) {
		similarity_detector_->reset();
	}

	// Release the camera if we opened one
	if (video_capture_) {
		try {
			video_capture_->release();
		} catch (const std::exception& exc) {
			log(LOG_WARNING, std::string("Releasing the camera failed: ") + exc.what());
		}
		video_capture_.reset();
	}

	// Close the auth ui if there is one
	if (ui_uid_.has_value()) {
		popup_.close(*ui_uid_);
		ui_uid_.reset();
	}
}

Preview Authenticator::preview(const std::string& username) const {
	Preview result;
	try {
		const std::unique_lock<std::mutex> lock(models_mutex_, std::try_to_lock);
		// It is loaded right now, the authentication looks again
		if (!lock.owns_lock()) {
			result.config_changed = true;
			return result;
		}
		const auto text = file_text(config_path_);
		result.config_changed = text.has_value() && text != config_text_;
		// The models in use may not be the ones the authentication takes
		if (result.config_changed || !backend_) {
			return result;
		}
		const auto loaded = load_user_encodings(username, backend_->encoding_size());
		result.has_face_model = loaded.has_value() && !loaded->empty();
	} catch (const std::exception&) {
		result.has_face_model = true;
	}
	return result;
}

Status Authenticator::run(const std::string& username, const Args& args, const RequestControl& control) { // NOLINT(readability-function-cognitive-complexity)
	// Amount of ignored dark frames
	int dark_tries = 0;
	double dark_running_total = 0;
	// Total amount of frames captured
	long frames = 0;
	int valid_frames = 0;
	// Frames whose best match already fell inside the threshold. One frame under
	// the bar is a weak signal: a scan produces dozens of independent looks, so
	// the frame that decides authentication is the *minimum* of many draws, and a
	// single blurred or oddly lit outlier can carry a stranger through. Demanding
	// several agreeing frames removes that outlier without moving the threshold.
	int confirmations_seen = 0;
	// The first frames of the scan for the snapshot, if enabled
	std::vector<cv::Mat> snapframes;
	// The lowest certainty value (distance) of all frames, for the snapshot
	double lowest_certainty = 10;
	// What the checks measure over several frames, for each way the frames are turned: a face has
	// another place and shape in a turned frame
	struct Tracked {
		dlib::rectangle place;
		std::optional<MicroMotionTracker> micromotion;
		std::optional<TextureDetector> texture;
	};
	std::map<int, Tracked> tracked;

	const auto send_to_ui = [&](const char* message, bool frame_counter) {
		// Only if the popup was requested
		if (!ui_uid_.has_value()) {
			return;
		}
		if (message != nullptr) {
			popup_.update_message(*ui_uid_, message);
		}
		if (frame_counter) {
			// Let the user know we're real busy
			std::string subtext = "Scanned " + std::to_string(valid_frames - dark_tries) + " frames";
			if (dark_tries > 1) {
				subtext += " (skipped " + std::to_string(dark_tries) + " dark frames)";
			}
			popup_.update_subtext(*ui_uid_, subtext);
		}
	};

	// Load the face model of the user
	const auto loaded = load_user_encodings(username, backend_->encoding_size());
	if (!loaded.has_value() || loaded->empty()) {
		return Status::NoEnrolledFaces;
	}
	const std::vector<Encoding>& encodings = *loaded;

	// Get the height of the image (which would be the width if screen is portrait oriented)
	double height = video_capture_->get(rotation_is_portrait(rotate_) ? cv::CAP_PROP_FRAME_WIDTH : cv::CAP_PROP_FRAME_HEIGHT);
	if (height == 0) {
		height = 1;
	}
	// Calculate the amount the image has to shrink
	double scaling_factor = max_height_ / height;
	if (scaling_factor == 0) {
		scaling_factor = 1;
	}

	// Initiate histogram equalization
	const auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));

	// Let the ui know that we're ready
	// send_to_ui(S("Face authentication..."), false);

	const auto start_time = Clock::now();
	while (!control.keepalive_expired()) {
		if (control.cancel.load()) {
			return Status::Cancelled;
		}

		// Increment the frame count every loop
		++frames;

		send_to_ui(nullptr, true);
		if (ui_uid_.has_value()) {
			// Also when the text stays the same, with black frames for example
			popup_.keep_alive(*ui_uid_);
		}

		// Stop if we've exceeded the time limit
		if (std::chrono::duration<double>(Clock::now() - start_time).count() > args.timeout) {
			// Create a timeout snapshot if enabled
			if (save_failed_) {
				make_snapshot(
					snapframes, S("FAILED"), std::chrono::duration<double>(Clock::now() - start_time).count(), frames, lowest_certainty, snapshot_retention_);
			}
			if (dark_tries == valid_frames) {
				log(LOG_ERR, std::string(S("All frames were too dark, please check dark_threshold in config\n")) + "Average darkness: " +
								 std::to_string(dark_running_total / std::max(1, valid_frames)) + ", Threshold: " + std::to_string(dark_threshold_));
				return Status::TooDark;
			}
			return Status::Timeout;
		}

		// Grab a single frame of video
		cv::Mat frame;
		cv::Mat gsframe;
		try {
			std::tie(frame, gsframe) = video_capture_->read_frame();
		} catch (const NoDeviceException& exc) {
			log(LOG_ERR, exc.what());
			return Status::Error;
		}

		// If snapshots have been turned on, start capturing frames for the snapshot
		if ((save_failed_ || save_successful_) && snapframes.size() < 3) {
			snapframes.push_back(frame.clone());
		}

		clahe->apply(gsframe, gsframe);

		// Create a histogram of the image with 8 values
		cv::Mat hist;
		const int channels = 0;
		const int hist_size = 8;
		const std::array<float, 2> range{0, 256};
		const float* ranges = range.data();
		cv::calcHist(&gsframe, 1, &channels, cv::Mat(), hist, 1, &hist_size, &ranges);
		// All values combined for percentage calculation
		const double hist_total = cv::sum(hist)[0];

		// If the image is fully black due to a bad camera read, skip to the next frame
		if (hist_total == 0) {
			continue;
		}

		// Calculate frame darkness
		const double darkness = hist.at<float>(0) / hist_total * 100;
		if (darkness == 100) {
			continue;
		}

		dark_running_total += darkness;
		++valid_frames;

		// If the image exceeds darkness threshold due to subject distance, skip to the next frame
		if (darkness > dark_threshold_) {
			++dark_tries;
			continue;
		}

		// If the height is too high
		if (scaling_factor != 1) {
			cv::resize(frame, frame, cv::Size(), scaling_factor, scaling_factor, cv::INTER_AREA);
			cv::resize(gsframe, gsframe, cv::Size(), scaling_factor, scaling_factor, cv::INTER_AREA);
		}

		// Check portrait in addition to landscape (1), portrait only (2) or
		// turn the frame by a fixed amount (3, 4)
		rotate_frame(frame, rotate_, static_cast<int>(frames));
		rotate_frame(gsframe, rotate_, static_cast<int>(frames));

#ifdef HOWDY_WITH_OPENVINO
		if (spoof_detector_ && (*spoof_detector_)(frame) > anti_spoof_threshold_) {
			continue;
		}
#endif

		// Get all faces from that frame, upsampled once
		const auto face_locations = backend_->detect(gsframe);

		// The distance of a face to the closest model, with OpenVINO the similarity to the most similar one
		const auto best_match = [&](const Encoding& face) {
			if (use_openvino_) {
				double best = -std::numeric_limits<double>::infinity();
				for (const auto& encoding : encodings) {
					best = std::max(best, std::inner_product(encoding.begin(), encoding.end(), face.begin(), 0.0));
				}
				return best;
			}
			double best = std::numeric_limits<double>::infinity();
			for (const auto& encoding : encodings) {
				double sum = 0;
				for (std::size_t i = 0; i < encoding.size(); ++i) {
					const double d = encoding[i] - face[i];
					sum += d * d;
				}
				best = std::min(best, std::sqrt(sum));
			}
			return best;
		};

		// One face per frame, the one that is most like the models: the liveness checks compare a frame
		// with the ones before, which only says something about one and the same person
		if (face_locations.empty()) {
			continue;
		}
		std::size_t chosen = 0;
		// Only computed here if there is a choice to make, the checks may reject the face before
		std::optional<Encoding> face_encoding;
		if (face_locations.size() > 1) {
			double chosen_value = 0;
			for (std::size_t i = 0; i < face_locations.size(); ++i) {
				auto candidate = backend_->encode(frame, face_locations[i]);
				if (!candidate.has_value()) {
					continue;
				}
				const double value = best_match(*candidate);
				if (!face_encoding.has_value() || (use_openvino_ ? value > chosen_value : value < chosen_value)) {
					chosen = i;
					chosen_value = value;
					face_encoding = std::move(candidate);
				}
			}
			if (!face_encoding.has_value()) {
				continue;
			}
		}
		const auto& fl = face_locations[chosen];

		const auto [entry, first_look] = tracked.try_emplace(applied_rotation(rotate_, static_cast<int>(frames)));
		Tracked& seen = entry->second;
		// Another place is another face, like a second picture next to the first one: the jump is no
		// movement, and what was measured is not about this one
		if (first_look || !seen.place.contains(dlib::center(fl))) {
			seen = Tracked{};
			if (use_micromotion_detection_) {
				seen.micromotion.emplace().init(micromotion_detection_buffer_size_);
			}
			if (use_texture_detection_) {
				seen.texture.emplace().init(texture_detection_buffer_size_);
			}
		}
		seen.place = fl;

#ifdef HOWDY_WITH_OPENVINO
		if (spoof_detector_flir_ && (*spoof_detector_flir_)(frame, fl) < damo_flir_threshold_) {
			continue;
		}
#endif

		if (seen.texture) {
			// No value means the face is outside of the frame, reject it
			const auto texture = (*seen.texture)(gsframe, fl);
			if (!texture.has_value() || *texture < texture_threshold_) {
				continue;
			}
		}

		if (seen.micromotion && seen.micromotion->calculate_displacement(backend_->motion_landmarks(frame, fl)) < micromotion_threshold_) {
			continue;
		}

		if (!face_encoding.has_value()) {
			face_encoding = backend_->encode(frame, fl);
			if (!face_encoding.has_value()) {
				continue;
			}
		}

		const double best = best_match(*face_encoding);
		// OpenVINO matches are a similarity, the lowest certainty is a distance
		lowest_certainty = std::min(lowest_certainty, use_openvino_ ? 1 - best : best);
		const bool accept = use_openvino_ ? certainty_ < best && best < 1 : 0 < best && best < certainty_;

		if (similarity_detector_) {
			const auto [similarity, stale] = (*similarity_detector_)(*face_encoding, identical_threshold_);
			if (stale || similarity < sudden_change_threshold_) {
				continue;
			}
		}

		// Check if a match that's confident enough
		if (accept) {
			++confirmations_seen;
			if (confirmations_seen < confirmations_) {
				// Keep scanning until enough separate frames agree
				continue;
			}

			// Resend the frame info once more before we terminate
			send_to_ui(nullptr, true);

			// Make snapshot if enabled
			if (save_successful_) {
				make_snapshot(
					snapframes, S("SUCCESSFUL"), std::chrono::duration<double>(Clock::now() - start_time).count(), frames, lowest_certainty,
					snapshot_retention_);
			}

			// Run the rubber stamps if enabled, they can still deny the authentication
			if (config_.get_bool("rubberstamps", "enabled", false)) {
				if (ui_uid_.has_value()) {
					popup_.update_subtext(*ui_uid_, "");
				}
				const StampContext stamp_context{
					config_,
					*video_capture_,
					*backend_,
					clahe,
					scaling_factor,
					applied_rotation(rotate_, static_cast<int>(frames)),
					control.cancel,
					[&](const std::string& message) {
						if (ui_uid_.has_value()) {
							popup_.update_message(*ui_uid_, message);
						}
					},
					[&](const std::string& subtext) {
						if (ui_uid_.has_value()) {
							popup_.update_subtext(*ui_uid_, subtext);
						}
					},
					[&] {
						if (ui_uid_.has_value()) {
							popup_.keep_alive(*ui_uid_);
						}
					},
					// The client has gone, nobody waits for the result any more
					[&] { return control.keepalive_expired(); },
				};
				switch (run_rubberstamps(stamp_context)) {
					case StampResult::Denied: return Status::NoMatch;
					case StampResult::Cancelled: return control.cancel.load() ? Status::Cancelled : Status::KeepAliveTimeout;
					case StampResult::Passed: break;
				}
			}

			return Status::Success;
		}
	}
	return Status::KeepAliveTimeout;
}

} // namespace howdy

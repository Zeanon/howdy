#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "config.hh"
#include "face_backend.hh"
#include "liveness.hh"
#include "snapshot.hh"
#include "video_capture.hh"

#ifdef HOWDY_WITH_OPENVINO
#include "openvino_backend.hh"
#endif

namespace howdy {

using Clock = std::chrono::steady_clock;

enum class Status {
	Running,
	Success,
	NoMatch,
	NoEnrolledFaces,
	Timeout,
	TooDark,
	Cancelled,
	Cancelling,
	Error,
	NotInitialized,
	NoDevice,
	KeepAliveTimeout,
};

// The strings are part of the D-Bus API, see pam/face_authenticator.cc
const char* to_string(Status status) noexcept;

// Receives the messages for the authentication popup
class PopupSink {
  public:
	PopupSink() = default;
	virtual ~PopupSink() = default;
	PopupSink(const PopupSink&) = delete;
	PopupSink& operator=(const PopupSink&) = delete;
	PopupSink(PopupSink&&) = delete;
	PopupSink& operator=(PopupSink&&) = delete;

	virtual void show(std::uint32_t uid, const std::string& message) = 0;
	virtual void update_message(std::uint32_t uid, const std::string& message) = 0;
	virtual void update_subtext(std::uint32_t uid, const std::string& subtext) = 0;
	// The popup hides itself after a while without a signal: call this while the
	// authentication runs, also when there is no new text. Cheap, it is rate limited.
	virtual void keep_alive(std::uint32_t uid) = 0;
	virtual void close(std::uint32_t uid) = 0;
};

// Per request state shared between the D-Bus handlers and the worker
struct RequestControl {
	std::atomic<bool> cancel{false};
	// The caller has no privileges: the timeout it asks for is limited. Set before the worker gets the request
	bool limit_timeout = false;
	// The config file is not the one that was loaded, see Authenticator::preview(). Set before
	// the worker gets the request
	bool config_changed = false;
	// The client has to poll GetResult regularly, otherwise the
	// authentication stops once this deadline is reached.
	std::atomic<Clock::rep> keepalive_deadline{0};

	void extend_keepalive(Clock::duration duration) {
		keepalive_deadline.store((Clock::now() + duration).time_since_epoch().count());
	}

	[[nodiscard]] bool keepalive_expired() const {
		return Clock::now().time_since_epoch().count() >= keepalive_deadline.load();
	}
};

// What is known about a request before it starts
struct Preview {
	// Whether the user has a face model, otherwise there is nothing to look for. True if that
	// can not be said, the authentication then reports what is wrong.
	bool has_face_model = true;
	// The config file was changed, the authentication has to load it first
	bool config_changed = false;
};

class Authenticator {
  public:
	// Throws ModelException (or other exceptions) if the models can not be loaded
	explicit Authenticator(PopupSink& popup);

	// args: show-auth-popup[=bool] and timeout=<time>. With control.config_changed the config is
	// loaded first, NotInitialized if that fails
	Status authenticate(const std::string& username, const std::vector<std::string>& args, const RequestControl& control);

	// Called for every request, the one place where the config file is compared
	[[nodiscard]] Preview preview(const std::string& username) const;

	// How long a caller without privileges has to wait after its authentication to start the next one
	[[nodiscard]] std::chrono::seconds start_cooldown() const {
		return start_cooldown_.load();
	}

  private:
	// The only settings of a request, everything else comes from the config file
	struct Args {
		bool auth_popup;
		double timeout;
	};

	// What decides which models are loaded: use_openvino, use_cnn, openvino_device, the two anti
	// spoof checks and whether landmarks are needed
	using ModelOptions = std::tuple<bool, bool, std::string, bool, bool, bool>;

	void read_config();
	// Apply config_, the models are only loaded if their options changed
	void load_settings();
	void initialize_models(bool landmarks);
	// Load the config file if it was changed or the last try failed, false if it fails now
	[[nodiscard]] bool load_if_needed(bool changed);
	[[nodiscard]] Args parse_args(const std::vector<std::string>& args, bool limit_timeout) const;
	Status run(const std::string& username, const Args& args, const RequestControl& control);
	void cleanup();

	PopupSink& popup_;
	std::mutex camera_lock_;

	// The config file as it was loaded last, to see whether it was changed
	std::string config_path_;
	std::optional<std::string> config_text_;
	Config config_;
	std::optional<ModelOptions> model_options_;
	// Held while the config and the models are replaced, and by preview(), which runs
	// on another thread
	mutable std::mutex models_mutex_;
	std::unique_ptr<VideoCapture> video_capture_;
	std::optional<std::uint32_t> ui_uid_;

	std::unique_ptr<FaceBackend> backend_;
#ifdef HOWDY_WITH_OPENVINO
	std::unique_ptr<SpoofDetector> spoof_detector_;
	std::unique_ptr<SpoofDetectorFlir> spoof_detector_flir_;
#endif
	std::optional<SimilarityDetector> similarity_detector_;

	// Config values
	bool use_cnn_ = false;
	bool use_openvino_ = false;
	double timeout_ = 4;
	// Read by the thread of the bus while another one loads the config
	std::atomic<std::chrono::seconds> start_cooldown_{std::chrono::seconds(2)};
	// The certainty of the config as a distance (dlib) or as a similarity (OpenVINO)
	double certainty_ = 75;
	int confirmations_ = 5;
	bool use_anti_spoof_ = true;
	double anti_spoof_threshold_ = 0.05;
	bool use_damo_flir_ = true;
	double damo_flir_threshold_ = 0.95;
	bool use_micromotion_detection_ = true;
	int micromotion_detection_buffer_size_ = 8;
	double micromotion_threshold_ = 0.03;
	bool use_texture_detection_ = true;
	int texture_detection_buffer_size_ = 5;
	double texture_threshold_ = 35.0;
	bool use_similarity_detection_ = true;
	int similarity_detection_buffer_size_ = 8;
	double identical_threshold_ = 0.9975;
	double sudden_change_threshold_ = 0.9;
	bool auth_popup_ = false;
	// [snapshots]
	bool save_failed_ = false;
	bool save_successful_ = false;
	SnapshotRetention snapshot_retention_;
	double dark_threshold_ = 50.0;
	double max_height_ = 320.0;
	int rotate_ = 0;
	std::string openvino_device_ = "GPU";
};

} // namespace howdy

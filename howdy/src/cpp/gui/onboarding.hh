#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <opencv2/videoio.hpp>

#include "gdbus_client.hh"
#include "model_enroller.hh"
#include "process.hh"

namespace gtkui {

// A camera of the system and what is known about it
struct CameraInfo {
	std::string name;
	std::string path;
	int score = 0;
	std::string recommendation;
	bool infrared = false;
};

// The setup wizard. The slides are:
//   0 welcome            4 is the infrared emitter flashing?
//   1 dlib data files    5 create a face model
//   2 OpenVINO data      6 certainty policy
//   3 find the camera    7 done
class Onboarding {
  public:
	struct Options {
		// Where the cameras are listed
		std::string v4l_directory = "/dev/v4l/by-path";
		// The program that writes the config as root, it is run with `set`. This one by default.
		std::string helper;
		// How long the steps of the face capture take, for tests
		howdy::EnrollTiming enroll_timing;
	};

	// Called with a title and the details if something failed, the wizard
	// ends after it. The default shows a dialog.
	using ErrorHandler = std::function<void(const std::string& title, const std::string& detail)>;

	explicit Onboarding(const std::string& glade_path, Options options);
	explicit Onboarding(const std::string& glade_path) : Onboarding(glade_path, Options{}) {
	}
	~Onboarding();

	Onboarding(const Onboarding&) = delete;
	Onboarding& operator=(const Onboarding&) = delete;
	Onboarding(Onboarding&&) = delete;
	Onboarding& operator=(Onboarding&&) = delete;

	[[nodiscard]] GtkWidget* window() const {
		return window_;
	}
	[[nodiscard]] GtkWidget* widget(const char* id) const;
	[[nodiscard]] int current_slide() const {
		return current_slide_;
	}
	[[nodiscard]] bool finished() const {
		return finished_;
	}
	[[nodiscard]] bool failed() const {
		return failed_;
	}

	void set_error_handler(ErrorHandler handler) {
		on_error_ = std::move(handler);
	}

	// The buttons of the wizard
	void go_next_slide();
	void quit();

  private:
	struct ScanResult;

	static void on_next(GtkButton* button, gpointer data);
	static void on_quit(GtkButton* button, gpointer data);
	static void on_scan_button(GtkButton* button, gpointer data);
	static void on_infrared_yes(GtkButton* button, gpointer data);
	static void on_infrared_no(GtkButton* button, gpointer data);
	static gboolean on_scanned(gpointer data);
	static gboolean on_window_closed(GtkWidget* widget, GdkEvent* event, gpointer data);

	void download_data_files(const std::string& directory, const std::string& marker, const char* label_id, const char* box_id, const char* done_text);
	void find_cameras();
	void show_devices(const std::vector<CameraInfo>& devices);
	void check_infrared_emitter();
	void configure_camera();
	void create_model();
	void finish();

	// The camera that is selected in the list of slide 3, false if there is none
	bool selected_camera(std::string& path, bool& infrared) const;
	void enable_next();
	[[nodiscard]] std::string helper_program() const;
	void show_error(const std::string& title, const std::string& detail = "");

	GtkBuilder* builder_ = nullptr;
	GtkWidget* window_ = nullptr;
	GtkWidget* next_button_ = nullptr;
	GtkWidget* slide_container_ = nullptr;
	GtkWidget* tree_view_ = nullptr;
	GtkWidget* creating_dialog_ = nullptr;
	std::vector<GtkWidget*> slides_;
	std::deque<std::string> output_lines_;
	cv::VideoCapture capture_;
	Options options_;
	ManagementClient client_;
	std::string user_ = session_user();
	ModelEnroller enroller_{client_, user_};
	int current_slide_ = 0;
	bool finished_ = false;
	bool failed_ = false;
	// Callbacks of the background work must not touch a destroyed window
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
	ErrorHandler on_error_;
};

} // namespace gtkui

#include "onboarding.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <opencv2/core.hpp>

#include "paths.hh"
#include "process.hh"

namespace gtkui {

namespace {

// The values of the certainty option that belong to the choices of the
// wizard. It is a percentage, the higher the stricter.
constexpr const char* CERTAINTY_FAST = "65";
constexpr const char* CERTAINTY_BALANCED = "75";
constexpr const char* CERTAINTY_SECURE = "88";

// How the cameras are rated
constexpr int SCORE_INFRARED = 5;
constexpr int SCORE_NOT_INFRARED = -5;
constexpr int SCORE_NOT_OPENED = -9;

constexpr std::size_t DOWNLOAD_LINES = 10;

std::string trim(const std::string& text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return "";
	}
	return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

void set_label(GtkWidget* label, const std::string& text) {
	gtk_label_set_text(GTK_LABEL(label), text.c_str());
}

// Black background for the output of the downloads
void make_terminal(GtkWidget* widget) {
	GtkCssProvider* css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css, "* { background-color: black; color: white; }", -1, nullptr);
	gtk_style_context_add_provider(gtk_widget_get_style_context(widget), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
}

// Cameras also have metadata nodes, only the ones that capture video are useful
bool is_capture_device(const std::string& path) {
	const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		// Let the regular checks report devices we can't query
		return true;
	}

	v4l2_capability capability{};
	const int r = ioctl(fd, VIDIOC_QUERYCAP, &capability);
	close(fd);
	if (r < 0) {
		return true;
	}

	// Use the capabilities of this specific device node if the driver provides them
	const __u32 capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? capability.device_caps : capability.capabilities;
	return (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
}

// Infrared cameras deliver gray images: every pixel has the same value in all channels
bool is_gray(const cv::Mat& frame) {
	if (frame.channels() != 3) {
		return frame.channels() == 1;
	}
	std::vector<cv::Mat> channels;
	cv::split(frame, channels);
	cv::Mat first;
	cv::Mat second;
	cv::absdiff(channels[0], channels[1], first);
	cv::absdiff(channels[1], channels[2], second);
	return cv::countNonZero(first) == 0 && cv::countNonZero(second) == 0;
}

// A better name than the path, from the udev database
std::string camera_name(const std::string& device_path, const std::string& fallback) {
	const gchar* argv[] = {"udevadm", "info", "-r", "--query=all", "-n", device_path.c_str(), nullptr};
	gchar* output = nullptr;
	if (g_spawn_sync(
			nullptr, const_cast<gchar**>(argv), nullptr, static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr, &output,
			nullptr, nullptr, nullptr) == 0) { // NOLINT(cppcoreguidelines-pro-type-const-cast)
		return fallback;
	}
	const std::string text = output != nullptr ? output : "";
	g_free(output);

	std::string name = fallback;
	static const std::regex PRODUCT("product.*=(.*)$", std::regex::icase);
	for (std::size_t start = 0; start < text.size();) {
		std::size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		const std::string line = text.substr(start, end - start);
		start = end + 1;

		std::smatch match;
		if (std::regex_search(line, match, PRODUCT)) {
			// The kernel cuts names off at 31 characters, which can leave a trailing colon
			std::string product = trim(match[1].str());
			while (!product.empty() && product.back() == ':') {
				product.pop_back();
			}
			name = trim(product);
		}
	}
	return name;
}

} // namespace

struct Onboarding::ScanResult {
	Onboarding* wizard;
	std::shared_ptr<bool> alive;
	// False if there is no list of cameras at all
	bool listed;
	std::vector<CameraInfo> devices;
};

namespace {

// Find all cameras and check if they are infrared. Opening every camera takes
// too long for the thread of the user interface. False if there is no list of
// cameras.
bool scan_cameras(const std::string& directory, std::vector<CameraInfo>& devices) {
	std::error_code ec;
	std::vector<std::string> names;
	for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
		names.push_back(entry.path().filename().string());
	}
	if (ec) {
		return false;
	}

	// Sorted so the same link is picked every time
	std::sort(names.begin(), names.end());

	// The directory can contain multiple links to the same device
	std::vector<std::filesystem::path> seen;

	for (const auto& name : names) {
		// The full path to the device is the default name
		const std::filesystem::path device_path = std::filesystem::path(directory) / name;

		const std::filesystem::path real_path = std::filesystem::canonical(device_path, ec);
		const std::filesystem::path identity = ec ? device_path : real_path;
		if (std::find(seen.begin(), seen.end(), identity) != seen.end()) {
			continue;
		}
		seen.push_back(identity);

		// Skip metadata nodes and other devices that can't capture video
		if (!is_capture_device(device_path.string())) {
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		CameraInfo device;
		device.name = camera_name(device_path.string(), name);
		device.path = device_path.string();

		cv::VideoCapture capture(device.path);
		cv::Mat frame;
		if (!capture.isOpened() || !capture.read(frame) || frame.empty()) {
			device.score = SCORE_NOT_OPENED;
			device.recommendation = S("No, camera can't be opened");
		} else if (!is_gray(frame)) {
			device.score = SCORE_NOT_INFRARED;
			device.recommendation = S("No, not an infrared camera");
		} else {
			device.score = SCORE_INFRARED;
			device.recommendation = S("Yes, compatible infrared camera");
			device.infrared = true;
		}
		capture.release();

		devices.push_back(std::move(device));
	}

	// Infrared cameras first
	std::stable_sort(devices.begin(), devices.end(), [](const CameraInfo& a, const CameraInfo& b) { return a.score > b.score; });
	return true;
}

} // namespace

Onboarding::Onboarding(const std::string& glade_path, Options options) : options_(std::move(options)) {
	enroller_.set_timing(options_.enroll_timing);

	builder_ = gtk_builder_new();

	GError* error = nullptr;
	if (gtk_builder_add_from_file(builder_, glade_path.c_str(), &error) == 0) {
		const std::string message = error != nullptr ? error->message : "unknown error";
		g_clear_error(&error);
		g_object_unref(builder_);
		throw std::runtime_error("Could not load " + glade_path + ": " + message);
	}

	gtk_builder_add_callback_symbols(
		builder_, "go_next_slide", G_CALLBACK(on_next), "exit", G_CALLBACK(on_quit), "on_scanbutton_click", G_CALLBACK(on_scan_button), "slide3_button_yes",
		G_CALLBACK(on_infrared_yes), "slide3_button_no", G_CALLBACK(on_infrared_no), nullptr);
	gtk_builder_connect_signals(builder_, this);

	window_ = widget("onboardingwindow");
	slide_container_ = widget("slidecontainer");
	next_button_ = widget("nextbutton");

	g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<Onboarding*>(data)->quit(); }), this);
	g_signal_connect(window_, "delete_event", G_CALLBACK(on_window_closed), this);

	for (int i = 0; i < 8; ++i) {
		slides_.push_back(widget(("slide" + std::to_string(i)).c_str()));
	}

	on_error_ = [this](const std::string& title, const std::string& detail) {
		GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title.c_str());
		gtk_window_set_title(GTK_WINDOW(dialog), S("Howdy Error"));
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
	};

	gtk_widget_show_all(window_);
	gtk_window_resize(GTK_WINDOW(window_), 500, 400);
}

Onboarding::~Onboarding() {
	*alive_ = false;
	capture_.release();
	if (window_ != nullptr && GTK_IS_WIDGET(window_)) { // NOLINT(bugprone-assignment-in-if-condition)
		g_signal_handlers_disconnect_by_data(window_, this);
		gtk_widget_destroy(window_);
	}
	g_object_unref(builder_);
}

GtkWidget* Onboarding::widget(const char* id) const {
	auto* object = gtk_builder_get_object(builder_, id);
	if (object == nullptr) {
		throw std::runtime_error(std::string("The glade file has no widget ") + id);
	}
	return GTK_WIDGET(object);
}

void Onboarding::on_next(GtkButton* /*button*/, gpointer data) {
	static_cast<Onboarding*>(data)->go_next_slide();
}

void Onboarding::on_quit(GtkButton* /*button*/, gpointer data) {
	static_cast<Onboarding*>(data)->quit();
}

gboolean Onboarding::on_window_closed(GtkWidget* /*widget*/, GdkEvent* /*event*/, gpointer data) {
	static_cast<Onboarding*>(data)->quit();
	return FALSE;
}

void Onboarding::quit() {
	capture_.release();
	gtk_main_quit();
}

std::string Onboarding::helper_program() const {
	return options_.helper.empty() ? self_program() : options_.helper;
}

void Onboarding::enable_next() {
	gtk_widget_set_sensitive(next_button_, TRUE);
	gtk_window_set_focus(GTK_WINDOW(window_), next_button_);
}

void Onboarding::show_error(const std::string& title, const std::string& detail) {
	failed_ = true;
	on_error_(title, detail);
	quit();
}

void Onboarding::go_next_slide() {
	if (current_slide_ + 1 >= static_cast<int>(slides_.size())) {
		return;
	}
	gtk_widget_set_sensitive(next_button_, FALSE);

	gtk_widget_hide(slides_[static_cast<std::size_t>(current_slide_)]);
	++current_slide_;
	gtk_widget_show(slides_[static_cast<std::size_t>(current_slide_)]);
	// The shown child may have zero or wrong dimensions
	gtk_widget_queue_resize(slide_container_);

	switch (current_slide_) {
		case 1:
			download_data_files(
				howdy::paths::DLIB_DATA_DIR, "shape_predictor_5_face_landmarks.dat", "downloadoutputlabel", "downloadeventbox",
				S("DLib datafiles have already been downloaded!\nClick Next to continue"));
			break;
		case 2:
			download_data_files(
				howdy::paths::OPENVINO_DATA_DIR, "face-detection-adas-0001.xml", "downloadoutputlabelopenvino", "downloadeventboxopenvino",
				S("OpenVINO datafiles have already been downloaded!\nClick Next to continue"));
			break;
		case 3: find_cameras(); break;
		case 4: check_infrared_emitter(); break;
		case 5: configure_camera(); break;
		case 6: enable_next(); break;
		case 7: finish(); break;
		default: break;
	}
}

void Onboarding::download_data_files(const std::string& directory, const std::string& marker, const char* label_id, const char* box_id, const char* done_text) {
	GtkWidget* label = widget(label_id);
	make_terminal(widget(box_id));

	if (std::filesystem::exists(std::filesystem::path(directory) / marker)) {
		set_label(label, done_text);
		enable_next();
		return;
	}

	// The data files are installed for the whole system
	output_lines_.clear();
	const auto alive = alive_;
	run_streaming(
		privileged({"/bin/sh", "-c", "cd " + std::string(g_shell_quote(directory.c_str())) + " && ./install.sh"}), "",
		[this, alive, label](const std::string& line) {
			if (!*alive) {
				return;
			}
			output_lines_.push_back(line);
			if (output_lines_.size() > DOWNLOAD_LINES) {
				output_lines_.pop_front();
			}
			std::string text;
			for (const auto& shown : output_lines_) {
				text += shown + "\n";
			}
			set_label(label, text);
		},
		[this, alive, label](int exit_code) {
			if (!*alive) {
				return;
			}
			if (exit_code != 0) {
				std::string text;
				for (const auto& shown : output_lines_) {
					text += shown + "\n";
				}
				show_error(S("Error while downloading datafiles"), text);
				return;
			}
			set_label(label, S("Done!\nClick Next to continue"));
			enable_next();
		});
}

void Onboarding::find_cameras() {
	const std::string directory = options_.v4l_directory;
	const auto alive = alive_;
	Onboarding* wizard = this;

	std::thread([directory, alive, wizard] {
		auto* result = new ScanResult{wizard, alive, false, {}}; // NOLINT(cppcoreguidelines-owning-memory)
		result->listed = scan_cameras(directory, result->devices);
		g_idle_add(on_scanned, result);
	}).detach();
}

gboolean Onboarding::on_scanned(gpointer data) {
	const std::unique_ptr<ScanResult> result(static_cast<ScanResult*>(data));
	if (!*result->alive) {
		return G_SOURCE_REMOVE;
	}

	if (!result->listed || result->devices.empty()) {
		result->wizard->show_error(
			S("No webcams found on system"), S("Please configure your camera yourself if you are sure a compatible camera is connected"));
	} else {
		result->wizard->show_devices(result->devices);
	}
	return G_SOURCE_REMOVE;
}

void Onboarding::show_devices(const std::vector<CameraInfo>& devices) {
	// The path and whether it is infrared are not shown
	GtkListStore* store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
	for (const auto& device : devices) {
		GtkTreeIter iter;
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(
			store, &iter, 0, device.name.c_str(), 1, device.recommendation.c_str(), 2, device.path.c_str(), 3, device.infrared ? TRUE : FALSE, -1);
	}

	tree_view_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	gtk_widget_set_vexpand(tree_view_, TRUE);

	int column_index = 0;
	for (const char* title : {S("Camera identifier or path"), S("Recommended")}) {
		GtkCellRenderer* cell = gtk_cell_renderer_text_new();
		g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view_), gtk_tree_view_column_new_with_attributes(title, cell, "text", column_index++, nullptr));
	}

	gtk_container_add(GTK_CONTAINER(widget("devicelistbox")), tree_view_);

	// The best camera is selected
	GtkTreePath* first = gtk_tree_path_new_first();
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree_view_), first, nullptr, FALSE);
	gtk_tree_path_free(first);

	gtk_widget_show(tree_view_);
	gtk_widget_hide(widget("loadinglabel"));
	enable_next();
}

bool Onboarding::selected_camera(std::string& path, bool& infrared) const {
	if (tree_view_ == nullptr) {
		return false;
	}

	GtkTreeModel* model = nullptr;
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view_)), &model, &iter) == 0) {
		return false;
	}

	gchar* device_path = nullptr;
	gboolean is_infrared = FALSE;
	gtk_tree_model_get(model, &iter, 2, &device_path, 3, &is_infrared, -1);
	path = device_path;
	infrared = is_infrared != FALSE;
	g_free(device_path);
	return true;
}

void Onboarding::check_infrared_emitter() {
	std::string path;
	bool infrared = false;
	if (!selected_camera(path, infrared)) {
		show_error(S("Error selecting camera"));
		return;
	}

	if (!infrared) {
		// The selected camera is not infrared, there is no emitter to check
		go_next_slide();
		return;
	}

	// Reading a frame turns the emitter on. The user clicks yes or no, which
	// calls on_infrared_yes or on_infrared_no.
	capture_.open(path);
	if (!capture_.isOpened()) {
		show_error(S("The selected camera cannot be opened"), S("Try to select another one"));
		return;
	}
	cv::Mat frame;
	capture_.read(frame);
}

void Onboarding::on_infrared_yes(GtkButton* /*button*/, gpointer data) {
	auto* self = static_cast<Onboarding*>(data);
	self->capture_.release();
	self->go_next_slide();
}

void Onboarding::on_infrared_no(GtkButton* /*button*/, gpointer data) {
	auto* self = static_cast<Onboarding*>(data);
	self->capture_.release();
	gtk_label_set_markup(
		GTK_LABEL(self->widget("leiestatus")),
		S("Please visit\n<a href=\"https://github.com/EmixamPP/linux-enable-ir-emitter\">https://github.com/EmixamPP/linux-enable-ir-emitter</a>\nto enable your ir emitter"));
	gtk_widget_hide(self->widget("leieyesbutton"));
	gtk_widget_hide(self->widget("leienobutton"));
}

void Onboarding::configure_camera() {
	std::string path;
	bool infrared = false;
	if (!selected_camera(path, infrared)) {
		show_error(S("Error selecting camera"));
		return;
	}

	// The scan can start when the camera is configured
	GtkWidget* scan_button = widget("scanbutton");
	gtk_widget_set_sensitive(scan_button, FALSE);

	const auto alive = alive_;
	run_async(privileged({helper_program(), "set", "device_path", path}), [this, alive, scan_button](const ProcessResult& result) {
		if (!*alive) {
			return;
		}
		if (result.exit_code != 0) {
			show_error(S("Error setting camera path"), std::string(S("Please set the camera path manually")) + "\n\n" + trim(result.text()));
			return;
		}
		gtk_widget_set_sensitive(scan_button, TRUE);
		gtk_window_set_focus(GTK_WINDOW(window_), scan_button);
	});
}

void Onboarding::on_scan_button(GtkButton* button, gpointer data) {
	auto* self = static_cast<Onboarding*>(data);
	self->create_model();
	gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
}

void Onboarding::create_model() {
	// Shown while the face recognition is loaded, which takes a moment
	creating_dialog_ =
		gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_OTHER, GTK_BUTTONS_NONE, "%s", S("Please look directly into the camera"));
	gtk_window_set_title(GTK_WINDOW(creating_dialog_), S("Creating Model"));
	gtk_widget_show_all(creating_dialog_);

	const auto alive = alive_;
	const auto close_notice = [this] {
		if (creating_dialog_ != nullptr) {
			gtk_widget_destroy(creating_dialog_);
			creating_dialog_ = nullptr;
		}
	};

	// The dialog that guides the user belongs to the wizard, the first model is number 0
	enroller_.enroll(
		GTK_WINDOW(window_), "Model #0",
		[this, alive, close_notice](const ModelEnroller::Result& result) {
			if (!*alive) {
				return;
			}
			close_notice();

			switch (result.outcome) {
				case ModelEnroller::Outcome::Saved: go_next_slide(); break;
				case ModelEnroller::Outcome::Cancelled:
					// The user closed the dialog: they can start the scan again
					gtk_widget_set_sensitive(widget("scanbutton"), TRUE);
					break;
				case ModelEnroller::Outcome::Failed: show_error(S("Can't save face model"), trim(result.error)); break;
			}
		},
		[alive, close_notice] {
			if (*alive) {
				close_notice();
			}
		});
}

void Onboarding::finish() {
	const char* certainty = CERTAINTY_BALANCED;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget("radiofast"))) != 0) {
		certainty = CERTAINTY_FAST;
	} else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget("radiosecure"))) != 0) {
		certainty = CERTAINTY_SECURE;
	}

	gtk_widget_hide(next_button_);
	gtk_widget_hide(widget("cancelbutton"));

	GtkWidget* finish_button = widget("finishbutton");
	gtk_widget_show(finish_button);
	gtk_window_set_focus(GTK_WINDOW(window_), finish_button);
	finished_ = true;

	const auto alive = alive_;
	run_async(privileged({helper_program(), "set", "certainty", certainty}), [this, alive](const ProcessResult& result) {
		if (!*alive) {
			return;
		}
		if (result.exit_code != 0) {
			show_error(S("Error setting certainty"), S("Certainty is set to the default value, Howdy setup is complete"));
		}
	});
}

} // namespace gtkui

// Drives the windows of the graphical interface without a person: buttons are
// clicked from here and the results are printed as "name: value" lines, which
// run_gui.sh checks. Needs a display (xvfb-run), the management daemon on a
// private bus and a camera, see run_gui.sh.
//
// Usage: gui_test main-window|main-window-add-cancel|video-tab|onboarding [options]
//   onboarding options: --v4l-dir <dir>  --certainty fast|balanced|secure
//                       --infrared-no    click "no" at the emitter question
//                       --add-cancels    the dialog of the face scan is closed
//                       --helper <path>  the program that sets the config as root
//
// Environment: FACE_VIDEO, BLANK_VIDEO (videos as cameras), FAKE_MODE_FILE
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include <unistd.h>

#include <gtk/gtk.h>

#include "main_window.hh"
#include "onboarding.hh"
#include "paths.hh"
#include "process.hh"

namespace {

using namespace std::chrono_literals;

// Run the main loop until the condition is true, false after the timeout
bool wait_for(const std::function<bool()>& condition, std::chrono::seconds timeout = 180s) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!condition()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		g_main_context_iteration(nullptr, FALSE);
		g_usleep(2000);
	}
	return true;
}

// The stand-in for the management daemon reads its mode from a file
void set_mode(const char* mode) {
	std::ofstream(std::getenv("FAKE_MODE_FILE")) << mode << "\n";
}

// The camera of the config: a video, or a device that does not exist
void write_config(const std::string& device, const char* plugin = "ffmpeg") {
	std::ofstream config(std::string(howdy::paths::CONFIG_DIR) + "/config.ini");
	config << "[video]\ndevice_path = " << device << "\nrecording_plugin = " << plugin << "\ndevice_format = mov\n";
}

std::string env(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr ? value : "";
}

// Short steps, the real ones take a minute
constexpr howdy::EnrollTiming SHORT_TIMING{0.5, 0.2, 1.0, 0.2};

void print_models(const gtkui::MainWindow& window) {
	std::cout << "models: " << window.models().size() << "\n";
	for (const auto& model : window.models()) {
		std::cout << "model: " << model.id << "|" << model.created << "|" << model.label << "\n";
	}
}

int test_main_window() {
	const std::string glade = std::string(howdy::paths::GTK_DATA_DIR) + "/main.glade";
	gtkui::MainWindow window(glade);
	window.set_enroll_timing(SHORT_TIMING);

	window.set_error_handler([](const std::string& title, const std::string& detail) { std::cout << "error: " << title << " | " << detail << "\n"; });
	window.set_fatal_handler([](const std::string& message) { std::cout << "fatal: " << message << "\n"; });

	const auto action = [&](const std::function<void(std::function<void()>)>& start) {
		bool done = false;
		// The callback is only called while this waits: it must not outlive `done`
		start([&done] { done = true; });
		if (!wait_for([&done] { return done; })) {
			std::cout << "timeout\n";
			std::exit(3);
		}
	};

	const auto shown = [&] { return gtk_widget_get_visible(window.window()) != FALSE ? "yes" : "no"; };
	// The time of a shown model in seconds, which is sent back with a change
	const auto seconds = [&](std::int64_t id) {
		for (const auto& model : window.models()) {
			if (model.number == id) {
				return model.seconds;
			}
		}
		return std::int64_t{-1};
	};

	std::cout << "window shown before the check: " << shown() << "\n";
	action([&](auto done) { window.start(done); });
	std::cout << "window shown after the check: " << shown() << "\n";
	std::cout << "authorized: " << (window.authorized() ? "yes" : "no") << "\n";
	print_models(window);

	std::cout << "step: add with a name\n";
	write_config(env("FACE_VIDEO"));
	action([&](auto done) { window.add_model("Test face", done); });
	print_models(window);

	std::cout << "step: add without a name\n";
	action([&](auto done) { window.add_model("", done); });
	print_models(window);

	std::cout << "step: delete\n";
	action([&](auto done) { window.delete_model(0, "Test face", seconds(0), done); });
	print_models(window);

	std::cout << "step: add without a camera\n";
	write_config("/dev/does-not-exist");
	action([&](auto done) { window.add_model("No camera", done); });
	std::cout << "busy after failure: " << (window.busy() ? "yes" : "no") << "\n";

	std::cout << "step: add without a face\n";
	write_config(env("BLANK_VIDEO"));
	action([&](auto done) { window.add_model("No face", done); });

	std::cout << "step: the daemon does not save the model\n";
	write_config(env("FACE_VIDEO"));
	set_mode("add-error");
	action([&](auto done) { window.add_model("Not saved", done); });
	set_mode("allow");

	std::cout << "step: delete fails\n";
	set_mode("remove-error");
	action([&](auto done) { window.delete_model(1, "Model #1", seconds(1), done); });
	set_mode("allow");

	std::cout << "step: rename\n";
	action([&](auto done) { window.rename_model(1, "Model #1", seconds(1), "Renamed", done); });
	print_models(window);

	std::cout << "step: rename fails\n";
	set_mode("rename-error");
	action([&](auto done) { window.rename_model(1, "Renamed", seconds(1), "Not renamed", done); });
	set_mode("allow");

	std::cout << "step: the list is out of date\n";
	action([&](auto done) { window.rename_model(1, "Another name", seconds(1), "Not renamed", done); });
	action([&](auto done) { window.delete_model(1, "Another name", seconds(1), done); });
	std::cout << "step: the time is not the one of the model\n";
	action([&](auto done) { window.rename_model(1, "Renamed", 5, "Not renamed", done); });
	action([&](auto done) { window.delete_model(1, "Renamed", 5, done); });
	print_models(window);

	// What the face is saved with is checked at the end
	action([&](auto done) { window.rename_model(1, "Renamed", seconds(1), "Model #1", done); });

	std::cout << "step: the video tab\n";
	write_config("/dev/video42", "opencv");
	auto* notebook = GTK_NOTEBOOK(gtk_builder_get_object(window.builder(), "notebook"));
	gtk_notebook_set_current_page(notebook, 1);
	const auto text = [&](const char* id) { return std::string(gtk_label_get_text(GTK_LABEL(gtk_builder_get_object(window.builder(), id)))); };
	std::cout << "camera: " << text("videoid") << " | " << text("videores") << " | " << text("videorecorder") << "\n";
	gtk_notebook_set_current_page(notebook, 0);

	std::cout << "step: the permission is refused later\n";
	set_mode("deny");
	action([&](auto done) { window.reload_models(done); });
	action([&](auto done) { window.delete_model(1, "Model #1", seconds(1), done); });
	std::cout << "still running after a refusal: yes\n";

	std::cout << "step: not allowed to manage the models\n";
	{
		gtkui::MainWindow denied_window(glade);
		denied_window.set_fatal_handler([](const std::string& message) { std::cout << "fatal: " << message << "\n"; });
		action([&](auto done) { denied_window.start(done); });
		std::cout << "denied: window shown: " << (gtk_widget_get_visible(denied_window.window()) != FALSE ? "yes" : "no") << "\n";
		std::cout << "denied: authorized: " << (denied_window.authorized() ? "yes" : "no") << "\n";
		set_mode("allow");
	}

	std::cout << "step: the list fails\n";
	{
		set_mode("list-error");
		gtkui::MainWindow failing_window(glade);
		failing_window.set_error_handler(
			[](const std::string& title, const std::string& detail) { std::cout << "error: " << title << " | " << detail << "\n"; });
		action([&](auto done) { failing_window.start(done); });
		// The permission was granted, the models just could not be listed
		std::cout << "failing: window shown: " << (gtk_widget_get_visible(failing_window.window()) != FALSE ? "yes" : "no") << "\n";
		set_mode("allow");
	}

	std::cout << "finished\n";
	return 0;
}

// The user closes the dialog of the face capture: that is no error. The dialog
// is closed from outside, see run_gui.sh.
int test_main_window_add_cancel() {
	gtkui::MainWindow window(std::string(howdy::paths::GTK_DATA_DIR) + "/main.glade");
	window.set_error_handler([](const std::string& title, const std::string& detail) { std::cout << "error: " << title << " | " << detail << "\n"; });

	bool started = false;
	window.start([&started] { started = true; });
	wait_for([&started] { return started; });

	write_config(env("FACE_VIDEO"));
	bool done = false;
	window.add_model("Cancelled", [&done] { done = true; });
	wait_for([&done] { return done; }, 60s);
	std::cout << "add finished: " << (done ? "yes" : "no") << "\n";
	std::cout << "models: " << window.models().size() << "\n";
	std::cout << "busy afterwards: " << (window.busy() ? "yes" : "no") << "\n";
	return 0;
}

// The video tab with a real camera (the one of the config): does a picture arrive?
int test_video_tab() {
	gtkui::MainWindow window(std::string(howdy::paths::GTK_DATA_DIR) + "/main.glade");
	window.set_error_handler([](const std::string& title, const std::string& detail) { std::cout << "error: " << title << " | " << detail << "\n"; });

	bool done = false;
	window.start([&done] { done = true; });
	wait_for([&done] { return done; });
	std::cout << "window shown: " << (gtk_widget_get_visible(window.window()) != FALSE ? "yes" : "no") << "\n";

	auto* notebook = GTK_NOTEBOOK(gtk_builder_get_object(window.builder(), "notebook"));
	gtk_notebook_set_current_page(notebook, 1);

	auto* image = GTK_IMAGE(gtk_builder_get_object(window.builder(), "opencvimage"));
	const bool arrived = wait_for([&] { return gtk_image_get_pixbuf(image) != nullptr; }, 5s);
	std::cout << "picture arrived: " << (arrived ? "yes" : "no") << "\n";
	if (arrived) {
		std::cout << "picture size: " << gdk_pixbuf_get_width(gtk_image_get_pixbuf(image)) << "x" << gdk_pixbuf_get_height(gtk_image_get_pixbuf(image)) << "\n";
	}

	// A picture that is set but never drawn is a black tab: count how often the
	// widget is drawn in three seconds. The pictures have to be drawn as they arrive.
	int draws = 0;
	g_signal_connect(
		image, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, gpointer count) -> gboolean {
			++*static_cast<int*>(count);
			return FALSE;
		}),
		&draws);
	wait_for([] { return false; }, 3s);
	std::cout << "picture redraws: " << draws << "\n";

	// Away from the tab the camera is released
	gtk_notebook_set_current_page(notebook, 0);
	return 0;
}

int test_onboarding(int argc, char** argv) {
	gtkui::Onboarding::Options options;
	std::string certainty = "balanced";
	bool infrared_no = false;
	bool add_cancels = false;
	for (int i = 2; i < argc; ++i) {
		if (std::strcmp(argv[i], "--v4l-dir") == 0 && i + 1 < argc) {
			options.v4l_directory = argv[++i];
		} else if (std::strcmp(argv[i], "--certainty") == 0 && i + 1 < argc) {
			certainty = argv[++i];
		} else if (std::strcmp(argv[i], "--infrared-no") == 0) {
			infrared_no = true;
		} else if (std::strcmp(argv[i], "--add-cancels") == 0) {
			add_cancels = true;
		} else if (std::strcmp(argv[i], "--helper") == 0 && i + 1 < argc) {
			options.helper = argv[++i];
		}
	}

	// The face is scanned with the camera of the config
	write_config(env("FACE_VIDEO"));
	options.enroll_timing = SHORT_TIMING;

	const std::string glade = std::string(howdy::paths::GTK_DATA_DIR) + "/onboarding.glade";
	gtkui::Onboarding wizard(glade, options);

	bool errored = false;
	wizard.set_error_handler([&](const std::string& title, const std::string& detail) {
		std::cout << "error: " << title << " | " << detail << "\n";
		errored = true;
	});

	const auto sensitive = [&](const char* id) { return gtk_widget_get_sensitive(wizard.widget(id)) != FALSE; };
	const auto visible = [&](const char* id) { return gtk_widget_get_visible(wizard.widget(id)) != FALSE; };
	const auto click = [&](const char* id) { gtk_button_clicked(GTK_BUTTON(wizard.widget(id))); };
	const auto stopped = [&] { return errored; };

	std::cout << "slide: " << wizard.current_slide() << "\n";

	// slides 0 to 3: welcome, the data files and the cameras
	for (int slide = 1; slide <= 3; ++slide) {
		click("nextbutton");
		std::cout << "slide: " << wizard.current_slide() << "\n";
		if (!wait_for([&] { return sensitive("nextbutton") || stopped(); }, 120s) || stopped()) {
			std::cout << "stopped at slide " << wizard.current_slide() << "\n";
			return 0;
		}
	}

	std::cout << "download output: " << gtk_label_get_text(GTK_LABEL(wizard.widget("downloadoutputlabel"))) << "\n";
	std::cout << "openvino output: " << gtk_label_get_text(GTK_LABEL(wizard.widget("downloadoutputlabelopenvino"))) << "\n";

	// slide 4: is the emitter flashing, only asked for infrared cameras
	click("nextbutton");
	std::cout << "slide: " << wizard.current_slide() << "\n";
	if (wizard.current_slide() == 4) {
		std::cout << "emitter question: yes\n";
		if (infrared_no) {
			click("leienobutton");
			std::cout << "emitter buttons hidden: " << (!visible("leieyesbutton") && !visible("leienobutton") ? "yes" : "no") << "\n";
			std::cout << "next is blocked: " << (!sensitive("nextbutton") ? "yes" : "no") << "\n";
			return 0;
		}
		click("leieyesbutton");
	} else {
		std::cout << "emitter question: skipped\n";
	}
	std::cout << "slide: " << wizard.current_slide() << "\n";

	// slide 5: the camera is configured, then the face scan can start
	if (!wait_for([&] { return sensitive("scanbutton") || stopped(); }) || stopped()) {
		std::cout << "stopped at slide " << wizard.current_slide() << "\n";
		return 0;
	}
	if (add_cancels) {
		// The dialog is closed from outside: no error, and the scan can be started again
		click("scanbutton");
		wait_for([&] { return !sensitive("scanbutton"); }, 2s);
		const bool usable_again = wait_for([&] { return sensitive("scanbutton") || stopped(); }, 60s);
		std::cout << "scan cancelled: stays on slide " << wizard.current_slide()
				  << ", scan button usable again: " << (usable_again && !stopped() ? "yes" : "no") << "\n";
		return 0;
	}
	click("scanbutton");
	if (!wait_for([&] { return wizard.current_slide() == 6 || stopped(); }) || stopped()) {
		std::cout << "stopped at slide " << wizard.current_slide() << "\n";
		return 0;
	}
	std::cout << "slide: " << wizard.current_slide() << "\n";

	// slide 6: the certainty policy
	if (certainty == "fast") {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wizard.widget("radiofast")), TRUE);
	} else if (certainty == "secure") {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wizard.widget("radiosecure")), TRUE);
	}
	if (!wait_for([&] { return sensitive("nextbutton"); })) {
		std::cout << "next never became available\n";
		return 0;
	}
	click("nextbutton");
	std::cout << "slide: " << wizard.current_slide() << "\n";
	std::cout << "finish button shown: " << (visible("finishbutton") ? "yes" : "no") << "\n";
	std::cout << "next button hidden: " << (!visible("nextbutton") ? "yes" : "no") << "\n";

	// The certainty is saved in the background
	wait_for([&] { return errored; }, 2s);
	click("finishbutton");
	std::cout << "finished\n";
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	setenv("GDK_BACKEND", "x11", 1);
	if (gtk_init_check(&argc, &argv) == FALSE) {
		std::cout << "error: no display\n";
		return 2;
	}
	if (chdir(howdy::paths::GTK_DATA_DIR) != 0) {
		return 2;
	}

	try {
		if (argc > 1 && std::strcmp(argv[1], "main-window") == 0) {
			return test_main_window();
		}
		if (argc > 1 && std::strcmp(argv[1], "main-window-add-cancel") == 0) {
			return test_main_window_add_cancel();
		}
		if (argc > 1 && std::strcmp(argv[1], "video-tab") == 0) {
			return test_video_tab();
		}
		if (argc > 1 && std::strcmp(argv[1], "onboarding") == 0) {
			return test_onboarding(argc, argv);
		}
	} catch (const std::exception& exc) {
		std::cout << "exception: " << exc.what() << "\n";
		return 1;
	}
	std::cout << "usage: gtk_test main-window|onboarding\n";
	return 2;
}

// Runs the guided capture of `howdy add` with short timings, to be run
// with a display (xvfb-run) and a camera configured in the test config.
//
// Usage: enroll_test [--with-parent] [--keep-open] [model file]
//   --keep-open    the finished capture is shown for a minute
//   --with-parent  the dialog belongs to a window of this program, whose X11 id
//                  is printed
//
// Prints the samples and, if a model file with a stored descriptor is given,
// the distance of the first sample to it.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

#include "config.hh"
#include "enroll_dialog.hh"
#include "face_backend.hh"
#include "paths.hh"
#include "video_capture.hh"

int main(int argc, char** argv) {
	setenv("GDK_BACKEND", "x11", 1);
	if (gtk_init_check(nullptr, nullptr) == FALSE) {
		std::cout << "error: no display" << std::endl;
		return 2;
	}

	// The window the dialog belongs to and the model to compare with
	bool with_parent = false;
	bool keep_open = false;
	const char* model_file = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--with-parent") {
			with_parent = true;
		} else if (std::string(argv[i]) == "--keep-open") {
			keep_open = true;
		} else {
			model_file = argv[i];
		}
	}

	try {
		const howdy::Config config((std::filesystem::path(howdy::paths::CONFIG_DIR) / "config.ini").string());
		const auto backend = howdy::create_dlib_backend(false);
		howdy::VideoCapture capture(config);

		howdy::EnrollSettings settings{
			*backend, capture, 60, [](const std::string& line) { std::cout << "say: " << line << std::endl; }, {0.5, 0.2, 1.0, keep_open ? 60.0 : 0.2},
		};
		GtkWidget* parent = nullptr;
		if (with_parent) {
			parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
			gtk_widget_show_all(parent);
			std::cout << "parent: 0x" << std::hex << gdk_x11_window_get_xid(gtk_widget_get_window(parent)) << std::dec << std::endl;
			settings.parent = GTK_WINDOW(parent);
		}
		const auto samples = howdy::capture_guided_samples(settings);

		std::cout << "samples: " << samples.size() << std::endl;

		if (model_file != nullptr && !samples.empty()) {
			std::ifstream file(model_file);
			const auto models = nlohmann::json::parse(file);
			const auto reference = models.at(0).at("data").at(0).get<howdy::Encoding>();
			double sum = 0;
			for (std::size_t i = 0; i < reference.size(); ++i) {
				sum += (reference[i] - samples[0][i]) * (reference[i] - samples[0][i]);
			}
			std::cout << "distance: " << std::sqrt(sum) << std::endl;
		}
	} catch (const howdy::EnrollCancelled&) {
		std::cout << "cancelled" << std::endl;
		return 1;
	} catch (const std::exception& exc) {
		std::cout << "error: " << exc.what() << std::endl;
		return 1;
	}
	return 0;
}

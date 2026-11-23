#include "gui.hh"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

#include <unistd.h>

#include <gtk/gtk.h>

#include "main_window.hh"
#include "onboarding.hh"
#include "paths.hh"

namespace gtkui {

int run_gui(bool force_onboarding) {
	// The position of the dialogs needs X11, XWayland is fine
	setenv("GDK_BACKEND", "x11", 1);
	if (gtk_init_check(nullptr, nullptr) == FALSE) {
		std::cerr << "Could not open a display\n";
		return 1;
	}

	// The glade files refer to the logos relative to the data directory
	const std::filesystem::path data_dir = howdy::paths::GTK_DATA_DIR;
	if (chdir(howdy::paths::GTK_DATA_DIR) != 0) {
		std::cerr << "Could not enter " << howdy::paths::GTK_DATA_DIR << ", the logos will be missing\n";
	}

	try {
		const std::string onboarding_glade = (data_dir / "onboarding.glade").string();

		if (force_onboarding) {
			Onboarding onboarding(onboarding_glade);
			gtk_main();
			return onboarding.failed() ? 1 : 0;
		}

		// Only the daemon can tell whether the user has models, the directory is not readable
		// for anybody else. Without models the onboarding replaces the window.
		std::unique_ptr<Onboarding> onboarding;
		MainWindow window((data_dir / "main.glade").string());
		window.set_no_models_handler([&] {
			try {
				onboarding = std::make_unique<Onboarding>(onboarding_glade);
			} catch (const std::exception& exc) {
				std::cerr << exc.what() << "\n";
				gtk_main_quit();
			}
		});
		// The window appears when polkit has allowed to manage the models
		window.start();
		gtk_main();
		if (onboarding != nullptr) {
			return onboarding->failed() ? 1 : 0;
		}
	} catch (const std::exception& exc) {
		std::cerr << exc.what() << "\n";
		return 1;
	}
	return 0;
}

} // namespace gtkui

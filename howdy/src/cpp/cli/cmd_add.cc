// howdy add: capture a face model and save it through the management daemon

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>

#include <unistd.h>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

#include "cli.hh"
#include "config.hh"
#include "enroll_dialog.hh"
#include "face_backend.hh"
#include "face_setup.hh"
#include "management_client.hh"
#include "model_limits.hh"
#include "paths.hh"
#include "util.hh"
#include "video_capture.hh"

namespace howdy::cli {

int run_add(const Options& options) {
	const BusPtr bus = open_system_bus();
	authorize(bus.get(), options);

	// Read config from disk
	const Config config((std::filesystem::path(paths::CONFIG_DIR) / "config.ini").string());

	std::unique_ptr<FaceBackend> backend;
	try {
		backend = create_face_backend(config, options.user, [&](const std::string& line) {
			if (!options.plain) {
				say(options, line);
			}
		});
	} catch (const std::exception& exc) {
		throw CliError(exc.what());
	}

	// If the daemon can not tell, start counting at 0
	std::vector<ModelInfo> models;
	try {
		models = list_models(bus.get(), options.user);
	} catch (const std::exception& exc) {
		log(LOG_DEBUG, std::string("Could not list the face models: ") + exc.what());
	}

	// Print a warning if too many encodings are being added
	if (many_models(models.size())) {
		say(options, std::string(S("NOTICE: ")) + many_models_warning());
		say(options, S("Press Ctrl+C to cancel\n"));
	}

	// Make clear what we are doing if not human
	if (!options.plain) {
		say(options, std::string(S("Adding face model for the user ")) + options.user);
	}

	std::string label = std::string(S("Model #")) + std::to_string(models.empty() ? 0 : models.back().id + 1);
	// Get the label from the cli arguments if provided
	if (!options.arguments.empty()) {
		label = options.arguments[0];
	}

	// Keep the default name if we can't ask questions (-y, or no TTY / closed stdin
	// as when the setup wizard / Settings runs add with the output captured).
	if (options.yes || isatty(STDIN_FILENO) == 0) {
		say(options, std::string(S("Using default label \"")) + label + S("\" because of -y flag"));
	} else {
		// Ask the user for a custom label and limit it to 24 characters
		const std::string label_in = ask(std::string(S("Enter a label for this new model [")) + label + "]: ");
		if (!label_in.empty()) {
			label = truncate_characters(label_in, 24);
		}
	}

	// Remove illegal characters
	if (label.find(',') != std::string::npos) {
		say(options, S("NOTICE: Removing illegal character \",\" from model name"));
		std::erase(label, ',');
	}

	// Set up video_capture
	std::unique_ptr<VideoCapture> video_capture;
	try {
		video_capture = std::make_unique<VideoCapture>(config);
	} catch (const NoDeviceException& exc) {
		throw CliError(exc.what());
	}

	// GTK needs X11 for the position of the dialog, XWayland is fine
	setenv("GDK_BACKEND", "x11", 1);
	if (gtk_init_check(nullptr, nullptr) == FALSE) {
		throw CliError(S("Could not open a display for the enrollment dialog"));
	}

	// Guide the user through small head movements and keep a few descriptors
	// per pose, so the model covers more than one snapshot
	std::vector<Encoding> samples;
	try {
		EnrollSettings settings{
			*backend,
			*video_capture,
			config.get_float("video", "dark_threshold", 60),
			[&](const std::string& line) { say(options, line); },
		};
		settings.rotate = static_cast<int>(config.get_int("video", "rotate", 0));
		samples = capture_guided_samples(settings);
	} catch (const EnrollCancelled&) {
		throw CliCancelled();
	} catch (const std::exception& exc) {
		throw CliError(exc.what());
	}

	// Release the camera right away
	video_capture.reset();

	if (samples.empty()) {
		throw CliError(S("No samples captured, aborting"));
	}
	say(options, std::string(S("Captured ")) + std::to_string(samples.size()) + " face samples");

	// Prepare the metadata for insertion
	const nlohmann::json model = {
		{"time", static_cast<long long>(std::time(nullptr))},
		{"label", label},
		{"data", samples},
	};

	try {
		add_model(bus.get(), options.user, model.dump());
	} catch (const std::exception& exc) {
		throw CliError(std::string("Failed to save face model: ") + exc.what());
	}

	// Let the user know how it went
	say(options, std::string("\n") + S("Scan complete") + "\n" + S("Added a new model to ") + options.user);
	return 0;
}

} // namespace howdy::cli

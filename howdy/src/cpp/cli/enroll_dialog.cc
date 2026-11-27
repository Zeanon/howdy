#include "enroll_dialog.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>

#include <cairo.h>
#include <gtk/gtk.h>

#include <opencv2/imgproc.hpp>

#include "rotation.hh"
#include "util.hh"

namespace howdy {

namespace {

using Clock = std::chrono::steady_clock;

// Guidance steps, small movements the frontal detector still handles
constexpr std::array<const char*, 6> GUIDE_STEPS{
	"Look straight at the camera", "Turn your head slightly to the left", "Turn your head slightly to the right",
	"Tilt your chin up a little",  "Tilt your chin down a little",		  "Look straight at the camera again",
};

// Per step: how many samples to keep. The time the user gets to move after
// each prompt (frames are read and discarded so the capture never sees a
// stale buffered frame) and to look for usable frames is in EnrollTiming.
constexpr std::size_t SAMPLES_PER_STEP = 2;
// Descriptors closer than this (euclidean) add nothing to the matcher and are not stored
constexpr double DUPLICATE_DISTANCE = 0.05;
// Safety cap on samples per model (auth cost is one norm per stored vector)
constexpr std::size_t MAX_SAMPLES = 1 + (GUIDE_STEPS.size() * SAMPLES_PER_STEP);
// The first frames are used to find a face
constexpr int FIRST_FACE_TRIES = 60;

// Highlight color 2AAAE5
constexpr const char* HIGHLIGHT_MARKUP = "#2AAAE5";
constexpr std::array<double, 3> HIGHLIGHT_RGB{42, 170, 229};

constexpr int DIALOG_WIDTH = 500;
constexpr int DIALOG_HEIGHT = 605;
// The preview is scaled down to fit into the dialog
constexpr int PREVIEW_MAX_WIDTH = 470;

struct Event {
	enum class Kind : std::uint8_t {
		// Text for the terminal
		Say,
		// Prompt for the user, shown in the dialog and on the terminal
		Guide,
		// Prompt for the dialog only
		GuideLabel,
		Progress,
		Finished,
	};

	Kind kind;
	std::string text;
	std::size_t count = 0;
	std::size_t total = 0;
};

// What the worker thread and the dialog share
struct Shared {
	std::mutex mutex;
	std::deque<Event> events;
	// The latest preview (RGB), empty if there is no new one
	cv::Mat preview;
	std::optional<std::string> error;
	bool cancelled = false;
	std::vector<Encoding> samples;
	std::atomic<bool> cancel{false};
	std::atomic<bool> finished{false};

	void post(Event event) {
		const std::lock_guard<std::mutex> lock(mutex);
		events.push_back(std::move(event));
	}

	void fail(const std::string& message) {
		{
			const std::lock_guard<std::mutex> lock(mutex);
			if (!error.has_value() && !cancelled) {
				error = message;
			}
		}
		finished = true;
	}

	void abort() {
		{
			const std::lock_guard<std::mutex> lock(mutex);
			// With the samples taken there is nothing left to cancel, only the wait ends
			if (!error.has_value() && samples.empty()) {
				cancelled = true;
			}
		}
		cancel = true;
		finished = true;
	}
};

double seconds_since(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

bool is_duplicate(const Encoding& descriptor, const std::vector<Encoding>& existing) {
	for (const auto& other : existing) {
		double sum = 0;
		for (std::size_t i = 0; i < descriptor.size() && i < other.size(); ++i) {
			const double d = descriptor[i] - other[i];
			sum += d * d;
		}
		if (std::sqrt(sum) < DUPLICATE_DISTANCE) {
			return true;
		}
	}
	return false;
}

// Percentage of pixels in the darkest of 8 histogram bins, nullopt for an empty frame
std::optional<double> frame_darkness(const cv::Mat& gray) {
	cv::Mat hist;
	const int channels = 0;
	const int hist_size = 8;
	const std::array<float, 2> range{0, 256};
	const float* ranges = range.data();
	cv::calcHist(&gray, 1, &channels, cv::Mat(), hist, 1, &hist_size, &ranges);
	const double total = cv::sum(hist)[0];
	if (total == 0) {
		return std::nullopt;
	}
	return hist.at<float>(0) / total * 100;
}

// Captures the samples, runs in its own thread
class Worker {
  public:
	Worker(const EnrollSettings& settings, Shared& shared) : settings_(settings), shared_(shared), clahe_(cv::createCLAHE(2.0, cv::Size(8, 8))) {
	}

	void run() {
		try {
			capture();
		} catch (const std::exception& exc) {
			shared_.fail(exc.what());
		}
		shared_.finished = true;
	}

  private:
	struct Frame {
		cv::Mat frame;
		std::vector<dlib::rectangle> faces;
	};

	[[nodiscard]] bool stopped() const {
		return shared_.cancel || shared_.finished;
	}

	// Read a frame, find the faces and publish the preview. The frame is empty
	// if the camera failed.
	Frame get_frame() {
		cv::Mat frame;
		cv::Mat gray;
		try {
			std::tie(frame, gray) = settings_.capture.read_frame();
		} catch (const NoDeviceException& exc) {
			shared_.fail(exc.what());
			return {};
		}

		// Turn sideways mounted cameras upright before anything else
		const int rotate = fixed_rotation(settings_.rotate);
		rotate_frame(frame, rotate, 0);
		rotate_frame(gray, rotate, 0);

		clahe_->apply(gray, gray);

		std::vector<dlib::rectangle> faces;
		cv::Mat overlay;

		// If the image is fully black due to a bad camera read, skip it
		const auto darkness = frame_darkness(gray);
		if (darkness.has_value() && *darkness != 100) {
			// Include this frame in calculating our average session brightness
			dark_running_total_ += *darkness;
			++valid_frames_;

			// If the image exceeds darkness threshold due to subject distance, skip it
			if (*darkness > settings_.dark_threshold) {
				++dark_tries_;
			} else {
				faces = settings_.backend.detect(gray);
				overlay = cv::Mat::zeros(gray.size(), CV_8UC3);
				for (const auto& face : faces) {
					const double half_width = static_cast<double>(face.right() - face.left()) / 2;
					const double half_height = static_cast<double>(face.bottom() - face.top()) / 2;
					cv::ellipse(
						overlay,
						cv::Point(static_cast<int>(half_width) + static_cast<int>(face.left()), static_cast<int>(half_height) + static_cast<int>(face.top())),
						cv::Size(static_cast<int>(half_width * 1.3), static_cast<int>(half_height * 1.3)), 0, 0, 360,
						cv::Scalar(HIGHLIGHT_RGB[0], HIGHLIGHT_RGB[1], HIGHLIGHT_RGB[2]), 3);
				}
			}
		}

		publish_preview(gray, overlay);
		return {frame, std::move(faces)};
	}

	void publish_preview(const cv::Mat& gray, const cv::Mat& overlay) {
		cv::Mat preview;
		cv::cvtColor(gray, preview, cv::COLOR_GRAY2RGB);

		if (!overlay.empty()) {
			// Blend the rings into the image
			const double alpha = 0.75;
			for (int y = 0; y < preview.rows; ++y) {
				auto* row = preview.ptr<cv::Vec3b>(y);
				const auto* ring = overlay.ptr<cv::Vec3b>(y);
				for (int x = 0; x < preview.cols; ++x) {
					if (ring[x][0] != 0 || ring[x][1] != 0 || ring[x][2] != 0) {
						for (int c = 0; c < 3; ++c) {
							row[x][c] = cv::saturate_cast<unsigned char>((ring[x][c] * alpha) + (row[x][c] * (1 - alpha)));
						}
					}
				}
			}
		}

		// Mirror it, like a mirror
		cv::flip(preview, preview, 1);
		if (preview.cols > PREVIEW_MAX_WIDTH) {
			const double scale = static_cast<double>(PREVIEW_MAX_WIDTH) / preview.cols;
			cv::resize(preview, preview, cv::Size(), scale, scale, cv::INTER_AREA);
		}

		const std::lock_guard<std::mutex> lock(shared_.mutex);
		shared_.preview = preview;
	}

	// Sleep for the time while the preview stays alive
	void keep_alive_for(double seconds) {
		const auto start = Clock::now();
		while (!stopped() && seconds_since(start) < seconds) {
			get_frame();
		}
	}

	Encoding encode(const Frame& frame, const dlib::rectangle& face) {
		auto encoding = settings_.backend.encode(frame.frame, face);
		if (!encoding.has_value()) {
			throw std::runtime_error(S("Failed to encode face, aborting"));
		}
		return std::move(*encoding);
	}

	void capture() {
		shared_.post({Event::Kind::Say, S("\nPlease look straight into the camera")});
		shared_.post({Event::Kind::GuideLabel, GUIDE_STEPS[0]});

		// Give the user time to read
		keep_alive_for(settings_.timing.init_wait);

		// Loop through frames till we hit a timeout
		Frame first;
		for (int i = 0; i < FIRST_FACE_TRIES && !stopped(); ++i) {
			first = get_frame();
			// If we've found at least one, we can continue
			if (!first.faces.empty()) {
				break;
			}
		}
		if (stopped()) {
			return;
		}

		// If we've found no faces, try to determine why
		if (first.faces.empty()) {
			if (valid_frames_ == 0) {
				throw std::runtime_error(S("Camera saw only black frames - is IR emitter working?"));
			}
			if (valid_frames_ == dark_tries_) {
				throw std::runtime_error(
					std::string(S("All frames were too dark, please check dark_threshold in config")) +
					" Average darkness: " + format_double(dark_running_total_ / valid_frames_) + ", Threshold: " + format_double(settings_.dark_threshold));
			}
			throw std::runtime_error(S("No face detected, aborting"));
		}

		// If more than 1 faces are detected we can't know which one belongs to the user
		if (first.faces.size() > 1) {
			throw std::runtime_error(S("Multiple faces detected, aborting"));
		}

		std::vector<Encoding> samples{encode(first, first.faces[0])};
		const std::size_t total = std::min(MAX_SAMPLES, 1 + (GUIDE_STEPS.size() * SAMPLES_PER_STEP));
		shared_.post({Event::Kind::Progress, "", samples.size(), total});

		for (const char* prompt : GUIDE_STEPS) {
			if (stopped() || samples.size() >= MAX_SAMPLES) {
				break;
			}

			shared_.post({Event::Kind::Guide, prompt});
			keep_alive_for(settings_.timing.settle);

			const auto step_start = Clock::now();
			std::size_t got = 0;
			while (!stopped() && got < SAMPLES_PER_STEP && samples.size() < MAX_SAMPLES && seconds_since(step_start) < settings_.timing.step) {
				const Frame frame = get_frame();
				if (frame.faces.size() != 1) {
					continue;
				}

				Encoding descriptor = encode(frame, frame.faces[0]);
				if (is_duplicate(descriptor, samples)) {
					continue;
				}

				samples.push_back(std::move(descriptor));
				++got;
				shared_.post({Event::Kind::Progress, "", samples.size(), total});
			}
		}

		if (shared_.cancel) {
			return;
		}

		const std::size_t count = samples.size();
		{
			const std::lock_guard<std::mutex> lock(shared_.mutex);
			shared_.samples = std::move(samples);
		}
		shared_.post({Event::Kind::Finished, "", count});

		// Let the user see the result, until the dialog is closed
		const auto shown_since = Clock::now();
		while (!shared_.cancel && seconds_since(shown_since) < settings_.timing.end_wait) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	const EnrollSettings& settings_;
	Shared& shared_;
	cv::Ptr<cv::CLAHE> clahe_;

	// Count the number of illuminated read frames
	int valid_frames_ = 0;
	// Count the number of illuminated frames that were rejected for being too dark
	int dark_tries_ = 0;
	// Track the running darkness total
	double dark_running_total_ = 0;
};

// The dialog, all calls have to come from the GTK thread
struct Dialog {
	Shared* shared = nullptr;
	const EnrollSettings* settings = nullptr;
	GtkWidget* window = nullptr;
	GtkWidget* guide_label = nullptr;
	GtkWidget* progress_label = nullptr;
	GtkWidget* image = nullptr;
	GMainLoop* loop = nullptr;
};

gboolean draw_background(GtkWidget* widget, cairo_t* ctx, gpointer /*data*/) {
	constexpr double radius = 18;
	constexpr double border = 2;
	constexpr double half = border / 2;

	// Make window transparent
	cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(ctx, 0, 0, 0, 0);
	cairo_paint(ctx);

	cairo_set_operator(ctx, CAIRO_OPERATOR_OVER);
	cairo_new_path(ctx);

	// left edge
	cairo_move_to(ctx, half, 0);
	cairo_line_to(ctx, half, DIALOG_HEIGHT - radius - half);
	// lower left corner
	cairo_arc_negative(ctx, radius + half, DIALOG_HEIGHT - radius - half, radius, M_PI, M_PI / 2);
	// lower edge
	cairo_line_to(ctx, DIALOG_WIDTH - radius - half, DIALOG_HEIGHT - half);
	// lower right corner
	cairo_arc_negative(ctx, DIALOG_WIDTH - radius - half, DIALOG_HEIGHT - radius - half, radius, M_PI / 2, 0);
	// right edge
	cairo_line_to(ctx, DIALOG_WIDTH - half, 0);

	// get the themes background color
	GdkRGBA background;
	if (gtk_style_context_lookup_color(gtk_widget_get_style_context(widget), "theme_bg_color", &background) != 0) {
		cairo_set_source_rgba(ctx, background.red, background.green, background.blue, background.alpha);
	} else {
		cairo_set_source_rgb(ctx, 0, 0, 0);
	}
	cairo_fill_preserve(ctx);

	cairo_set_line_width(ctx, border);
	cairo_set_line_join(ctx, CAIRO_LINE_JOIN_ROUND);
	cairo_set_source_rgb(ctx, HIGHLIGHT_RGB[0] / 255, HIGHLIGHT_RGB[1] / 255, HIGHLIGHT_RGB[2] / 255);
	cairo_stroke(ctx);

	// Let the children draw themselves
	return FALSE;
}

void set_markup(GtkWidget* label, const std::string& text, bool big) {
	gchar* escaped = g_markup_escape_text(text.c_str(), -1);
	gchar* markup =
		g_strdup_printf(big ? "<big><span foreground='%s'><b>%s</b></span></big>" : "<span foreground='%s'><b>%s</b></span>", HIGHLIGHT_MARKUP, escaped);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	g_free(escaped);
}

void on_cancel(GtkWidget* /*button*/, gpointer data) {
	static_cast<Dialog*>(data)->shared->abort();
}

void on_response(GtkDialog* /*dialog*/, gint /*response*/, gpointer data) {
	static_cast<Dialog*>(data)->shared->abort();
}

// Escape and the window manager: only cancel. The window stays until the worker is done,
// it is updated until then and destroyed afterwards
gboolean on_delete(GtkWidget* /*window*/, GdkEvent* /*event*/, gpointer data) {
	static_cast<Dialog*>(data)->shared->abort();
	return TRUE;
}

// Show the latest frame and handle what the worker reports
gboolean update_ui(gpointer data) {
	auto* dialog = static_cast<Dialog*>(data);
	Shared& shared = *dialog->shared;

	cv::Mat preview;
	std::deque<Event> events;
	{
		const std::lock_guard<std::mutex> lock(shared.mutex);
		preview = shared.preview;
		shared.preview = cv::Mat();
		events.swap(shared.events);
	}

	if (!preview.empty()) {
		// The pixbuf owns a copy of the pixels
		auto* holder = new cv::Mat(preview.clone()); // NOLINT(cppcoreguidelines-owning-memory)
		GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(
			holder->data, GDK_COLORSPACE_RGB, FALSE, 8, holder->cols, holder->rows, static_cast<int>(holder->step),
			[](guchar* /*pixels*/, gpointer user) { delete static_cast<cv::Mat*>(user); }, holder);
		gtk_image_set_from_pixbuf(GTK_IMAGE(dialog->image), pixbuf);
		g_object_unref(pixbuf);
	}

	for (const auto& event : events) {
		switch (event.kind) {
			case Event::Kind::Say: dialog->settings->say(event.text); break;
			case Event::Kind::Guide:
				set_markup(dialog->guide_label, event.text, true);
				dialog->settings->say(event.text);
				break;
			case Event::Kind::GuideLabel: set_markup(dialog->guide_label, event.text, true); break;
			case Event::Kind::Progress:
				set_markup(dialog->progress_label, "Samples: " + std::to_string(event.count) + "/" + std::to_string(event.total), false);
				break;
			case Event::Kind::Finished: set_markup(dialog->progress_label, "Captured " + std::to_string(event.count) + " face samples", false); break;
		}
	}

	if (shared.finished) {
		g_main_loop_quit(dialog->loop);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

void build_dialog(Dialog& dialog) {
	GtkWidget* window = gtk_dialog_new();
	dialog.window = window;
	gtk_window_set_title(GTK_WINDOW(window), "Howdy – Create Model");
	g_signal_connect(window, "draw", G_CALLBACK(draw_background), nullptr);

	gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
	gtk_window_set_default_size(GTK_WINDOW(window), DIALOG_WIDTH, DIALOG_HEIGHT);
	gtk_widget_set_app_paintable(window, TRUE);
	gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
	gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_widget_set_can_focus(window, FALSE);
	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
	gtk_window_set_modal(GTK_WINDOW(window), TRUE);

	GdkScreen* screen = gdk_screen_get_default();
	GdkVisual* visual = screen != nullptr ? gdk_screen_get_rgba_visual(screen) : nullptr;
	if (visual != nullptr) {
		gtk_widget_set_visual(window, visual);
	}

	// Top center of the primary monitor, below the camera
	GdkDisplay* display = gdk_display_get_default();
	GdkMonitor* monitor = display != nullptr ? gdk_display_get_primary_monitor(display) : nullptr;
	if (monitor == nullptr && display != nullptr) {
		monitor = gdk_display_get_monitor(display, 0);
	}
	if (monitor != nullptr) {
		GdkRectangle geometry;
		gdk_monitor_get_geometry(monitor, &geometry);
		gtk_window_move(GTK_WINDOW(window), geometry.x + ((geometry.width - DIALOG_WIDTH) / 2), geometry.y);
	}

	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	GtkWidget* inner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	dialog.guide_label = gtk_label_new(nullptr);
	gtk_label_set_line_wrap(GTK_LABEL(dialog.guide_label), TRUE);
	gtk_box_pack_start(GTK_BOX(inner_box), dialog.guide_label, FALSE, FALSE, 0);

	dialog.progress_label = gtk_label_new(nullptr);
	gtk_style_context_add_class(gtk_widget_get_style_context(dialog.progress_label), "dim-label");
	gtk_box_pack_start(GTK_BOX(inner_box), dialog.progress_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), inner_box, FALSE, FALSE, 30);

	dialog.image = gtk_image_new();
	gtk_widget_set_hexpand(dialog.image, TRUE);
	gtk_box_pack_start(GTK_BOX(box), dialog.image, TRUE, TRUE, 0);

	GtkWidget* abort_button = gtk_button_new_with_label("Cancel");
	gtk_widget_set_hexpand(abort_button, FALSE);
	g_signal_connect(abort_button, "clicked", G_CALLBACK(on_cancel), &dialog);
	GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(button_box), abort_button, TRUE, TRUE, 75);
	gtk_box_pack_start(GTK_BOX(box), button_box, FALSE, FALSE, 20);

	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(window))), box);
	g_signal_connect(window, "response", G_CALLBACK(on_response), &dialog);
	g_signal_connect(window, "delete-event", G_CALLBACK(on_delete), &dialog);

	// Belong to the window that asked for the dialog
	if (dialog.settings->parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(window), dialog.settings->parent);
	}
}

} // namespace

std::vector<Encoding> capture_guided_samples(const EnrollSettings& settings) {
	Shared shared;
	Dialog dialog;
	dialog.shared = &shared;
	dialog.settings = &settings;

	build_dialog(dialog);
	gtk_widget_show_all(dialog.window);

	dialog.loop = g_main_loop_new(nullptr, FALSE);
	g_timeout_add(33, update_ui, &dialog);

	Worker worker(settings, shared);
	std::thread thread([&worker] { worker.run(); });

	g_main_loop_run(dialog.loop);

	// The worker stops at the next frame
	shared.cancel = true;
	thread.join();
	g_main_loop_unref(dialog.loop);
	gtk_widget_destroy(dialog.window);

	if (shared.cancelled) {
		throw EnrollCancelled();
	}
	if (shared.error.has_value()) {
		throw std::runtime_error(*shared.error);
	}
	return std::move(shared.samples);
}

} // namespace howdy

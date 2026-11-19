#include "video_preview.hh"

#include <algorithm>
#include <filesystem>

#include <INIReader.h>
#include <opencv2/imgproc.hpp>

#include "paths.hh"
#include "process.hh"

namespace gtkui {

namespace {

constexpr double MAX_HEIGHT = 300;
constexpr double MAX_WIDTH = 300;

// Reading a frame blocks until the camera delivers it, which takes longer than
// this. The next frame is only scheduled when the last one is done, and after
// the redrawing of the window (G_PRIORITY_DEFAULT_IDLE is behind it). A timer
// that repeats every 20 ms would always be due again and the window would never
// be drawn: the picture is set, but the tab stays black.
constexpr guint FRAME_INTERVAL_MSEC = 20;

void set_text(GtkBuilder* builder, const char* id, const std::string& text) {
	if (auto* label = GTK_LABEL(gtk_builder_get_object(builder, id)); label != nullptr) {
		gtk_label_set_text(label, text.c_str());
	}
}

// Free the pixels together with the pixbuf
void free_frame(guchar* /*pixels*/, gpointer data) {
	delete static_cast<cv::Mat*>(data); // NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace

VideoPreview::VideoPreview(GtkBuilder* builder) : builder_(builder), image_(GTK_WIDGET(gtk_builder_get_object(builder, "opencvimage"))) {
	// Give the video preview a black background
	GtkCssProvider* css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css, "* { background-color: black; }", -1, nullptr);
	if (auto* box = GTK_WIDGET(gtk_builder_get_object(builder_, "opencvbox")); box != nullptr) {
		gtk_style_context_add_provider(gtk_widget_get_style_context(box), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	g_object_unref(css);
}

VideoPreview::~VideoPreview() {
	stop();
}

void VideoPreview::start() {
	stop();

	const INIReader config((std::filesystem::path(howdy::paths::CONFIG_DIR) / "config.ini").string());
	const std::string path = config.Get("video", "device_path", "");

	set_text(builder_, "videoid", std::filesystem::path(path).filename().string());
	set_text(builder_, "videorecorder", config.Get("video", "recording_plugin", S("Unknown")));

	capture_.open(path, cv::CAP_V4L);
	if (!capture_.isOpened()) {
		set_text(builder_, "videores", S("Can't open camera"));
		set_text(builder_, "videoresused", "");
		return;
	}

	// Brightness is changed while the preview runs and restored afterwards
	if (config.HasValue("video", "brightness") && !config.Get("video", "brightness", "").empty()) {
		original_brightness_ = capture_.get(cv::CAP_PROP_BRIGHTNESS);
		capture_.set(cv::CAP_PROP_BRIGHTNESS, static_cast<double>(config.GetInteger("video", "brightness", 0)));
	}

	const double height = std::max(capture_.get(cv::CAP_PROP_FRAME_HEIGHT), 1.0);
	const double width = std::max(capture_.get(cv::CAP_PROP_FRAME_WIDTH), 1.0);

	scaling_factor_ = MAX_HEIGHT / height;
	if (width * scaling_factor_ > MAX_WIDTH) {
		scaling_factor_ = MAX_WIDTH / width;
	}

	const double config_scaling = config.GetReal("video", "max_height", 320.0) / height;
	set_text(builder_, "videores", std::to_string(static_cast<int>(width)) + "x" + std::to_string(static_cast<int>(height)));
	set_text(
		builder_, "videoresused", std::to_string(static_cast<int>(width * config_scaling)) + "x" + std::to_string(static_cast<int>(height * config_scaling)));

	timer_ = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, FRAME_INTERVAL_MSEC, on_tick, this, nullptr);
}

void VideoPreview::stop() {
	if (timer_ != 0) {
		g_source_remove(timer_);
		timer_ = 0;
	}
	if (capture_.isOpened()) {
		if (original_brightness_.has_value()) {
			capture_.set(cv::CAP_PROP_BRIGHTNESS, *original_brightness_);
		}
		capture_.release();
	}
	original_brightness_.reset();
}

gboolean VideoPreview::on_tick(gpointer data) {
	auto* self = static_cast<VideoPreview*>(data);

	// This timer fires once, the next one is set when the frame is done
	self->timer_ = 0;
	if (self->show_frame()) {
		self->timer_ = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, FRAME_INTERVAL_MSEC, on_tick, self, nullptr);
	}
	return G_SOURCE_REMOVE;
}

bool VideoPreview::show_frame() {
	if (!capture_.isOpened()) {
		return false;
	}

	cv::Mat frame;
	if (!capture_.read(frame) || frame.empty()) {
		// A frame can be missing now and then
		return true;
	}

	cv::flip(frame, frame, 1);
	cv::resize(frame, frame, cv::Size(), scaling_factor_, scaling_factor_, cv::INTER_AREA);

	if (frame.channels() == 1) {
		cv::cvtColor(frame, frame, cv::COLOR_GRAY2RGB);
	} else {
		cv::cvtColor(frame, frame, cv::COLOR_BGR2RGB);
	}

	auto* pixels = new cv::Mat(frame); // NOLINT(cppcoreguidelines-owning-memory)
	GdkPixbuf* pixbuf =
		gdk_pixbuf_new_from_data(pixels->data, GDK_COLORSPACE_RGB, FALSE, 8, pixels->cols, pixels->rows, static_cast<int>(pixels->step), free_frame, pixels);
	gtk_image_set_from_pixbuf(GTK_IMAGE(image_), pixbuf);
	g_object_unref(pixbuf);
	return true;
}

} // namespace gtkui

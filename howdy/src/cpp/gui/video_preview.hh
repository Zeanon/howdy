#pragma once

#include <optional>
#include <string>

#include <gtk/gtk.h>
#include <opencv2/videoio.hpp>

namespace gtkui {

// The live image of the camera on the video tab, with the details of the
// configured camera.
class VideoPreview {
  public:
	explicit VideoPreview(GtkBuilder* builder);
	~VideoPreview();

	VideoPreview(const VideoPreview&) = delete;
	VideoPreview& operator=(const VideoPreview&) = delete;
	VideoPreview(VideoPreview&&) = delete;
	VideoPreview& operator=(VideoPreview&&) = delete;

	// Open the camera of the config and show it. Reads the config again.
	void start();

	// Release the camera
	void stop();

	[[nodiscard]] bool running() const {
		return capture_.isOpened();
	}

  private:
	static gboolean on_tick(gpointer data);
	bool show_frame();

	GtkBuilder* builder_;
	GtkWidget* image_;
	cv::VideoCapture capture_;
	std::optional<double> original_brightness_;
	double scaling_factor_ = 1;
	guint timer_ = 0;
};

} // namespace gtkui

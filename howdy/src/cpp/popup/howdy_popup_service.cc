
#include <cairo.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include <syslog.h>
#include <unistd.h>

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static constexpr const char* BUS_NAME = "@authentication_bus@";
static constexpr const char* POPUP_PATH = "@popup_path@";
static constexpr const char* POPUP_INTERFACE = "@popup_interface@";
static constexpr const char* LOGO_PATH = "@data_dir@/assets/logo_popup.png";

static constexpr const char* SCREENSAVER_PATH = "/org/freedesktop/ScreenSaver";

static constexpr const char* SCREENSAVER_INTERFACE = "org.freedesktop.ScreenSaver";

static constexpr int WINDOW_WIDTH = 400;
static constexpr int WINDOW_HEIGHT = 100;

static constexpr double HIGHLIGHT_R = 42.0 / 255.0;
static constexpr double HIGHLIGHT_G = 170.0 / 255.0;
static constexpr double HIGHLIGHT_B = 229.0 / 255.0;

using Clock = std::chrono::steady_clock;

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------

static void log_message(int priority, const std::string& message) {
	syslog(priority, "%s", message.c_str());
}

static void log_gerror(const char* message, GError* error) {
	std::string result(message);

	if (error) {
		result += ": ";
		result += error->message;
		g_error_free(error);
	}

	log_message(LOG_ERR, result);
}

// -----------------------------------------------------------------------------
// Popup window
// -----------------------------------------------------------------------------

class PopupWindow final {
  public:
	explicit PopupWindow(const std::string& message) : message_(message), deadline_(Clock::now() + std::chrono::seconds(2)) {
		logo_ = cairo_image_surface_create_from_png(LOGO_PATH);

		if (cairo_surface_status(logo_) != CAIRO_STATUS_SUCCESS) {
			const std::string error_message = cairo_status_to_string(cairo_surface_status(logo_));

			cairo_surface_destroy(logo_);
			logo_ = nullptr;

			throw std::runtime_error("Unable to load popup logo: " + error_message);
		}

		const int logo_height = cairo_image_surface_get_height(logo_);

		if (logo_height <= 0)
			throw std::runtime_error("Invalid popup logo dimensions");

		logo_scale_ = static_cast<double>(WINDOW_HEIGHT - 28) / static_cast<double>(logo_height);

		window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);

		if (!window_)
			throw std::runtime_error("Unable to create GTK window");

		gtk_window_set_title(GTK_WINDOW(window_), "Howdy Authentication");

		gtk_window_set_type_hint(GTK_WINDOW(window_), GDK_WINDOW_TYPE_HINT_NOTIFICATION);

		gtk_window_set_default_size(GTK_WINDOW(window_), WINDOW_WIDTH, WINDOW_HEIGHT);

		gtk_window_set_gravity(GTK_WINDOW(window_), GDK_GRAVITY_STATIC);

		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window_), true);

		gtk_window_set_skip_pager_hint(GTK_WINDOW(window_), true);

		gtk_window_set_accept_focus(GTK_WINDOW(window_), false);

		gtk_window_set_focus_on_map(GTK_WINDOW(window_), false);

		gtk_window_set_keep_above(GTK_WINDOW(window_), true);

		gtk_window_set_resizable(GTK_WINDOW(window_), false);

		gtk_window_set_decorated(GTK_WINDOW(window_), false);

		gtk_widget_set_app_paintable(window_, true);

		gtk_widget_set_can_focus(window_, false);

		gtk_widget_set_can_default(window_, false);

		gtk_window_set_focus(GTK_WINDOW(window_), nullptr);

		// Enable transparent window background.
		GdkScreen* screen = gtk_widget_get_screen(window_);

		if (screen) {
			GdkVisual* visual = gdk_screen_get_rgba_visual(screen);

			if (visual) {
				gtk_widget_set_visual(window_, visual);
			}
		}

		drawing_area_ = gtk_drawing_area_new();

		gtk_widget_set_size_request(drawing_area_, WINDOW_WIDTH, WINDOW_HEIGHT);

		gtk_widget_set_app_paintable(drawing_area_, true);

		g_signal_connect(drawing_area_, "draw", G_CALLBACK(on_draw), this);

		gtk_container_add(GTK_CONTAINER(window_), drawing_area_);

		position_window();

		gtk_widget_show_all(window_);

		start_timeout();
	}

	~PopupWindow() {
		if (timeout_id_ != 0) {
			g_source_remove(timeout_id_);
			timeout_id_ = 0;
		}

		if (window_) {
			gtk_widget_destroy(window_);
			window_ = nullptr;
		}

		if (logo_) {
			cairo_surface_destroy(logo_);
			logo_ = nullptr;
		}
	}

	void show_message(const std::string& message) {
		closed_ = false;
		message_ = message;
		subtext_.clear();
		deadline_ = Clock::now() + std::chrono::seconds(2);

		gtk_widget_queue_draw(drawing_area_);

		gtk_widget_show_all(window_);

		start_timeout();
	}

	void set_message(const std::string& message) {
		message_ = message;
		refresh();
	}

	void set_subtext(const std::string& subtext) {
		subtext_ = subtext;
		refresh();
	}

	// Closed by the daemon, updates that come later do not show it any more
	void hide() {
		closed_ = true;

		if (window_)
			gtk_widget_hide(window_);

		if (timeout_id_ != 0) {
			g_source_remove(timeout_id_);
			timeout_id_ = 0;
		}
	}

  private:
	GtkWidget* window_ = nullptr;
	GtkWidget* drawing_area_ = nullptr;

	cairo_surface_t* logo_ = nullptr;

	std::string message_;
	std::string subtext_;

	double logo_scale_ = 1.0;

	Clock::time_point deadline_;

	guint timeout_id_ = 0;

	bool closed_ = false;

	// An update: draw it, and show the window again if it has hidden itself in a pause
	void refresh() {
		if (closed_)
			return;

		deadline_ = Clock::now() + std::chrono::seconds(1);

		gtk_widget_queue_draw(drawing_area_);
		gtk_widget_show_all(window_);
		start_timeout();
	}

	void position_window() {
		GdkDisplay* display = gdk_display_get_default();

		if (!display)
			return;

		GdkMonitor* monitor = gdk_display_get_primary_monitor(display);

		if (!monitor && gdk_display_get_n_monitors(display) > 0) {
			monitor = gdk_display_get_monitor(display, 0);
		}

		if (!monitor)
			return;

		GdkRectangle geometry;

		gdk_monitor_get_geometry(monitor, &geometry);

		const int x = geometry.x + (geometry.width - WINDOW_WIDTH) / 2;

		const int y = geometry.y;

		gtk_window_move(GTK_WINDOW(window_), x, y);
	}

	void start_timeout() {
		if (timeout_id_ != 0) {
			g_source_remove(timeout_id_);
			timeout_id_ = 0;
		}

		timeout_id_ = g_timeout_add(100, on_timeout, this);
	}

	static gboolean on_timeout(gpointer user_data) {
		auto* self = static_cast<PopupWindow*>(user_data);

		if (Clock::now() >= self->deadline_) {
			self->timeout_id_ = 0;

			gtk_widget_hide(self->window_);

			return G_SOURCE_REMOVE;
		}

		return G_SOURCE_CONTINUE;
	}

	static void draw_rounded_background(cairo_t* cr) {
		constexpr double radius = 17.0;
		constexpr double border = 2.0;
		constexpr double half = border / 2.0;

		// Draw background.
		cairo_new_path(cr);

		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, WINDOW_WIDTH, 0);
		cairo_line_to(cr, WINDOW_WIDTH, WINDOW_HEIGHT - radius);

		cairo_arc(cr, WINDOW_WIDTH - radius, WINDOW_HEIGHT - radius, radius, 0, G_PI / 2.0);

		cairo_line_to(cr, radius, WINDOW_HEIGHT);

		cairo_arc(cr, radius, WINDOW_HEIGHT - radius, radius, G_PI / 2.0, G_PI);

		cairo_close_path(cr);

		cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
		cairo_fill(cr);

		// Draw border without top edge.
		cairo_new_path(cr);

		cairo_move_to(cr, half, 0);
		cairo_line_to(cr, half, WINDOW_HEIGHT - radius);

		cairo_arc_negative(cr, radius + half, WINDOW_HEIGHT - radius - half, radius, G_PI, G_PI / 2.0);

		cairo_line_to(cr, WINDOW_WIDTH - radius, WINDOW_HEIGHT - half);

		cairo_arc_negative(cr, WINDOW_WIDTH - radius - half, WINDOW_HEIGHT - radius - half, radius, G_PI / 2.0, 0);

		cairo_line_to(cr, WINDOW_WIDTH - half, 0);

		cairo_set_line_width(cr, border);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_source_rgb(cr, HIGHLIGHT_R, HIGHLIGHT_G, HIGHLIGHT_B);

		cairo_stroke(cr);
	}

	static void draw_text(
		cairo_t* cr, const std::string& text, double x, double y, double max_width, double font_size, double red, double green, double blue, double alpha) {
		if (text.empty())
			return;

		PangoLayout* layout = pango_cairo_create_layout(cr);

		PangoFontDescription* font = pango_font_description_from_string("Arial");

		pango_font_description_set_absolute_size(font, font_size * PANGO_SCALE);

		pango_layout_set_font_description(layout, font);

		pango_layout_set_text(layout, text.c_str(), -1);

		pango_layout_set_width(layout, static_cast<int>(max_width * PANGO_SCALE));

		pango_layout_set_single_paragraph_mode(layout, true);

		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		cairo_set_source_rgba(cr, red, green, blue, alpha);

		cairo_move_to(cr, x, y);

		pango_cairo_show_layout(cr, layout);

		pango_font_description_free(font);
		g_object_unref(layout);
	}

	static gboolean on_draw(GtkWidget*, cairo_t* cr, gpointer user_data) {
		auto* self = static_cast<PopupWindow*>(user_data);

		// Clear transparent background.
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);

		cairo_paint(cr);

		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

		draw_rounded_background(cr);

		// Draw logo.
		cairo_save(cr);

		cairo_translate(cr, 19.0, 10.0);

		cairo_scale(cr, self->logo_scale_, self->logo_scale_);

		cairo_set_source_surface(cr, self->logo_, 0.0, 0.0);

		cairo_paint(cr);

		cairo_restore(cr);

		constexpr double text_x = 109.0;
		constexpr double text_width = WINDOW_WIDTH - text_x - 20.0;

		if (self->subtext_.empty()) {
			draw_text(
				cr, self->message_, text_x,
				39.0, // y-wert
				text_width, 20.0, 1.0, 1.0, 1.0, 0.9);
		} else {
			draw_text(
				cr, self->message_, text_x,
				26.0, // y-wert
				text_width, 20.0, 1.0, 1.0, 1.0, 0.9);

			draw_text(
				cr, self->subtext_, text_x,
				52.0, // y-wert
				text_width, 12.0, 0.9, 0.9, 0.9, 0.8);
		}

		return true;
	}
};

// -----------------------------------------------------------------------------
// Popup service
// -----------------------------------------------------------------------------

class PopupService final {
  public:
	PopupService() : uid_(getuid()) {
		loop_ = g_main_loop_new(nullptr, false);
	}

	~PopupService() {
		if (system_bus_ && system_subscription_ != 0) {
			g_dbus_connection_signal_unsubscribe(system_bus_, system_subscription_);
		}

		if (session_bus_ && session_subscription_) {
			g_dbus_connection_signal_unsubscribe(session_bus_, session_subscription_);
		}

		if (unlock_timeout_id_ != 0)
			g_source_remove(unlock_timeout_id_);

		popup_.reset();

		if (system_bus_)
			g_object_unref(system_bus_);

		if (session_bus_)
			g_object_unref(session_bus_);

		if (loop_)
			g_main_loop_unref(loop_);
	}

	int run() {
		if (!connect_buses())
			return 1;

		log_message(LOG_INFO, "Howdy popup service is ready");

		g_main_loop_run(loop_);

		return exit_code_;
	}

	// The screen was unlocked: the service is to be set up anew
	[[nodiscard]] bool restart_requested() const {
		return restart_;
	}

  private:
	const uid_t uid_;

	bool restart_ = false;

	GMainLoop* loop_ = nullptr;

	GDBusConnection* system_bus_ = nullptr;
	GDBusConnection* session_bus_ = nullptr;

	guint system_subscription_ = 0;
	guint session_subscription_ = 0;

	guint unlock_timeout_id_ = 0;

	std::unique_ptr<PopupWindow> popup_;

	int exit_code_ = 0;

	bool connect_buses() {
		GError* error = nullptr;

		system_bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);

		if (!system_bus_) {
			log_gerror("Unable to connect to system bus", error);

			return false;
		}

		session_bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);

		if (!session_bus_) {
			log_gerror("Unable to connect to session bus", error);

			return false;
		}

		system_subscription_ = g_dbus_connection_signal_subscribe(
			system_bus_, BUS_NAME, POPUP_INTERFACE, nullptr, POPUP_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_popup_signal, this, nullptr);

		session_subscription_ = g_dbus_connection_signal_subscribe(
			session_bus_, nullptr, SCREENSAVER_INTERFACE, "ActiveChanged", SCREENSAVER_PATH, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver_signal, this,
			nullptr);

		if (system_subscription_ == 0 || session_subscription_ == 0) {
			log_message(LOG_ERR, "Unable to subscribe to D-Bus signals");

			return false;
		}

		return true;
	}

	static void on_popup_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* member, GVariant* parameters, gpointer user_data) {
		auto* self = static_cast<PopupService*>(user_data);

		if (!self || !member || !parameters)
			return;

		const std::string method(member);

		if (method == "Show" || method == "UpdateMessage" || method == "UpdateSubtext") {
			if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)")))
				return;

			guint32 uid = 0;
			const gchar* value = nullptr;

			g_variant_get(parameters, "(u&s)", &uid, &value);

			if (uid != self->uid_)
				return;

			const std::string text(value ? value : "");

			if (method == "Show")
				self->show(text);
			else if (method == "UpdateMessage")
				self->update_message(text);
			else
				self->update_subtext(text);
		} else if (method == "Close") {
			if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)")))
				return;

			guint32 uid = 0;

			g_variant_get(parameters, "(u)", &uid);

			if (uid != self->uid_)
				return;

			self->close();
		}
	}

	static void on_screensaver_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer user_data) {
		auto* self = static_cast<PopupService*>(user_data);

		if (!self || !parameters)
			return;

		if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)")))
			return;

		gboolean locked = false;

		g_variant_get(parameters, "(b)", &locked);

		self->on_screen_state_changed(locked);
	}

	void show(const std::string& message) {
		try {
			if (!popup_) {
				popup_ = std::make_unique<PopupWindow>(message);
			} else {
				popup_->show_message(message);
			}
		} catch (const std::exception& exception) {
			log_message(LOG_ERR, std::string("Failed to initialize authentication popup: ") + exception.what());

			exit_code_ = 1;
			g_main_loop_quit(loop_);
		}
	}

	void update_message(const std::string& message) {
		if (popup_)
			popup_->set_message(message);
	}

	void update_subtext(const std::string& subtext) {
		if (popup_)
			popup_->set_subtext(subtext);
	}

	void close() {
		if (popup_)
			popup_->hide();
	}

	void on_screen_state_changed(bool locked) {
		if (unlock_timeout_id_ != 0) {
			g_source_remove(unlock_timeout_id_);
			unlock_timeout_id_ = 0;
		}

		if (!locked) {
			unlock_timeout_id_ = g_timeout_add(500, restart_service, this);
		}
	}

	static gboolean restart_service(gpointer user_data) {
		auto* self = static_cast<PopupService*>(user_data);

		self->unlock_timeout_id_ = 0;

		log_message(LOG_INFO, "Screen unlocked; restarting authentication popup");

		// Ends run(), main() sets up a new service
		self->restart_ = true;
		g_main_loop_quit(self->loop_);

		return G_SOURCE_REMOVE;
	}
};

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
	openlog("howdy-auth-popup", LOG_PID, LOG_AUTH);

	g_setenv("GDK_BACKEND", "x11", true);

	if (!gtk_init_check(&argc, &argv)) {
		log_message(LOG_ERR, "GTK initialization failed: no accessible display");

		closelog();
		return 1;
	}

	try {
		// A new service after every unlock of the screen, in this process: systemd does
		// not have to start it again
		int result = 0;
		bool restart = true;
		while (restart) {
			PopupService service;

			result = service.run();
			restart = service.restart_requested();
		}

		closelog();
		return result;
	} catch (const std::exception& exception) {
		log_message(LOG_ERR, std::string("Fatal popup service error: ") + exception.what());

		closelog();
		return 1;
	}
}

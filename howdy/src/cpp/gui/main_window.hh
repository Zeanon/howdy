#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "gdbus_client.hh"
#include "model_enroller.hh"
#include "video_preview.hh"

namespace gtkui {

struct ModelRow {
	std::string id;
	std::string created;
	std::string label;
};

// The list of the management daemon, a JSON list of models with "id", "time"
// (seconds since 1970) and "label". Throws std::runtime_error.
std::vector<ModelRow> parse_model_list(const std::string& json);

// The configuration window: the face models, the camera and the about page
class MainWindow {
  public:
	// Called with a title and the details if something failed. The default
	// shows a dialog.
	using ErrorHandler = std::function<void(const std::string& title, const std::string& detail)>;

	explicit MainWindow(const std::string& glade_path);
	~MainWindow();

	MainWindow(const MainWindow&) = delete;
	MainWindow& operator=(const MainWindow&) = delete;
	MainWindow(MainWindow&&) = delete;
	MainWindow& operator=(MainWindow&&) = delete;

	[[nodiscard]] GtkWidget* window() const {
		return window_;
	}
	[[nodiscard]] GtkBuilder* builder() const {
		return builder_;
	}
	[[nodiscard]] const std::vector<ModelRow>& models() const {
		return models_;
	}
	[[nodiscard]] bool busy() const {
		return busy_;
	}

	void set_error_handler(ErrorHandler handler) {
		on_error_ = std::move(handler);
	}

	// The user is not allowed to manage the models: the window is useless
	void set_fatal_handler(std::function<void(const std::string&)> handler) {
		on_fatal_ = std::move(handler);
	}

	// Check that the user may manage the models and show the window if so.
	// Until then there is no window, polkit may ask for a password first. If it
	// is denied the window never appears, see set_fatal_handler().
	void start(std::function<void()> done = {});

	[[nodiscard]] bool authorized() const {
		return authorized_;
	}

	// How long the steps of the face capture take, for tests
	void set_enroll_timing(howdy::EnrollTiming timing) {
		enroller_.set_timing(timing);
	}

	// The actions of the buttons. They run in the background, `done` is called
	// when they are finished.
	// If the user is not allowed to manage the models, it is fatal while the
	// window is started and an error later.
	void reload_models(std::function<void()> done = {}, bool denied_is_fatal = false);
	// An empty name keeps the default one
	void add_model(const std::string& name, std::function<void()> done = {});
	void delete_model(const std::string& id, std::function<void()> done = {});

  private:
	static void on_page_switch(GtkNotebook* notebook, GtkWidget* page, guint page_number, gpointer data);
	static void on_model_add(GtkButton* button, gpointer data);
	static void on_model_delete(GtkButton* button, gpointer data);
	static gboolean on_about_link(GtkLabel* label, gchar* uri, gpointer data);
	static void on_destroy(GtkWidget* widget, gpointer data);

	void ask_for_model_name();
	void ask_to_delete_model();
	void set_busy(bool busy);
	[[nodiscard]] std::string next_model_name() const;
	static std::string session_user_name();
	void error(const std::string& title, const std::string& detail);

	GtkBuilder* builder_ = nullptr;
	GtkWidget* window_ = nullptr;
	GtkWidget* tree_view_ = nullptr;
	GtkListStore* list_store_ = nullptr;
	std::unique_ptr<VideoPreview> video_;
	ManagementClient client_;
	std::string user_ = session_user_name();
	ModelEnroller enroller_{client_, user_};
	std::vector<ModelRow> models_;
	bool busy_ = false;
	bool authorized_ = false;
	// Callbacks of the background work must not touch a destroyed window
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

	ErrorHandler on_error_;
	std::function<void(const std::string&)> on_fatal_;
};

} // namespace gtkui

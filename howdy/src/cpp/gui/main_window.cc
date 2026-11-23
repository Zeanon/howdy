#include "main_window.hh"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "process.hh"

namespace gtkui {

namespace {

std::string trim(const std::string& text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return "";
	}
	return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

} // namespace

std::vector<ModelRow> parse_model_list(const std::string& json) {
	const auto models = nlohmann::json::parse(json, nullptr, false);
	if (models.is_discarded() || !models.is_array()) {
		throw std::runtime_error("The list of the models is not valid");
	}

	std::vector<ModelRow> rows;
	for (const auto& model : models) {
		if (!model.is_object() || !model.contains("id") || !model.contains("time") || !model.contains("label")) {
			throw std::runtime_error("The list of the models is not valid");
		}

		// Shown in the local time zone, like `howdy list`
		const auto seconds = static_cast<std::time_t>(model.at("time").get<double>());
		std::tm local{};
		localtime_r(&seconds, &local);
		std::array<char, 32> created{};
		std::strftime(created.data(), created.size(), "%Y-%m-%d %H:%M:%S", &local);

		rows.push_back({std::to_string(model.at("id").get<long long>()), created.data(), model.at("label").get<std::string>()});
	}
	return rows;
}

MainWindow::MainWindow(const std::string& glade_path) {
	builder_ = gtk_builder_new();

	GError* error = nullptr;
	if (gtk_builder_add_from_file(builder_, glade_path.c_str(), &error) == 0) {
		const std::string message = error != nullptr ? error->message : "unknown error";
		g_clear_error(&error);
		g_object_unref(builder_);
		throw std::runtime_error("Could not load " + glade_path + ": " + message);
	}

	gtk_builder_add_callback_symbols(
		builder_, "on_page_switch", G_CALLBACK(on_page_switch), "on_model_add", G_CALLBACK(on_model_add), "on_about_link", G_CALLBACK(on_about_link), nullptr);
	gtk_builder_connect_signals(builder_, this);

	window_ = GTK_WIDGET(gtk_builder_get_object(builder_, "mainwindow"));

	// The tabs draw their own top border, which would be a second line below the
	// separator on top of the window
	GtkCssProvider* tab_css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(tab_css, "header.left tab { border-top-width: 0; }", -1, nullptr);
	gtk_style_context_add_provider(
		gtk_widget_get_style_context(GTK_WIDGET(gtk_builder_get_object(builder_, "notebook"))), GTK_STYLE_PROVIDER(tab_css),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(tab_css);
	g_signal_connect(window_, "destroy", G_CALLBACK(on_destroy), this);

	// GTK3 has no glade property for rounded corners, so use CSS. The box paints
	// the (rounded) background, the treeview on top of it stays transparent
	GtkCssProvider* radius_css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(
		radius_css,
		"box.modellistbox { border: 1px solid @borders; border-radius: 8px; background-color: @theme_base_color; }"
		"box.modellistbox treeview.view { background-color: transparent; border-radius: 0; }"
		"box.modellistbox treeview.view.cell { background-color: transparent; }"
		"box.modellistbox treeview.view.cell:hover { background-color: alpha(@theme_selected_bg_color, 0.35); }"
		"box.modellistbox treeview.view.cell:selected { background-color: @theme_selected_bg_color; }"
		"box.modellistbox treeview.view.cell, box.modellistbox treeview.view.cell:selected, box.modellistbox treeview.view.cell:hover, box.modellistbox treeview header button { border-radius: 0; }"
		"box.modellistbox treeview header button:first-child { border-top-left-radius: 7px; }"
		"box.modellistbox treeview header button:last-child { border-top-right-radius: 7px; }",
		-1, nullptr);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(gtk_builder_get_object(builder_, "modellistbox"))), "modellistbox");
	gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(window_), GTK_STYLE_PROVIDER(radius_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(radius_css);

	// A list of the models
	tree_view_ = gtk_tree_view_new();
	gtk_widget_set_vexpand(tree_view_, TRUE);
	int column_index = 0;
	for (const char* title : {S("ID"), S("Created"), S("Label")}) {
		GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
		// The IDs are set in a little. As a padding of the cell, not as a space in the text: the text is
		// the ID that is sent to remove the model, and the daemon compares the IDs as text.
		if (column_index == 0) {
			g_object_set(renderer, "xpad", 6, nullptr);
		}
		GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(title, renderer, "text", column_index++, nullptr);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view_), column);
	}
	list_store_ = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view_), GTK_TREE_MODEL(list_store_));
	gtk_container_add(GTK_CONTAINER(gtk_builder_get_object(builder_, "modellistbox")), tree_view_);

	build_model_menu();

	video_ = std::make_unique<VideoPreview>(builder_);

	on_error_ = [this](const std::string& title, const std::string& detail) {
		GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title.c_str());
		gtk_window_set_title(GTK_WINDOW(dialog), S("Howdy Error"));
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
	};
	on_fatal_ = [](const std::string& message) {
		g_printerr("%s\n", message.c_str());
		std::exit(1);
	};
}

MainWindow::~MainWindow() {
	*alive_ = false;
	video_.reset();
	if (window_ != nullptr && GTK_IS_WIDGET(window_)) { // NOLINT(bugprone-assignment-in-if-condition)
		g_signal_handlers_disconnect_by_data(window_, this);
		gtk_widget_destroy(window_);
	}
	g_object_unref(list_store_);
	g_object_unref(builder_);
}

void MainWindow::on_destroy(GtkWidget* /*widget*/, gpointer data) {
	auto* self = static_cast<MainWindow*>(data);
	*self->alive_ = false;
	self->video_.reset();
	self->window_ = nullptr;
	gtk_main_quit();
}

void MainWindow::on_page_switch(GtkNotebook* /*notebook*/, GtkWidget* /*page*/, guint page_number, gpointer data) {
	auto* self = static_cast<MainWindow*>(data);
	// The video tab is the second one
	if (page_number == 1) {
		self->video_->start();
	} else {
		self->video_->stop();
	}
}

void MainWindow::on_model_add(GtkButton* /*button*/, gpointer data) {
	static_cast<MainWindow*>(data)->ask_for_model_name();
}

gboolean MainWindow::on_tree_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
	auto* self = static_cast<MainWindow*>(data);
	if (event->type != GDK_BUTTON_PRESS) {
		return FALSE;
	}

	// Any other click on the list closes the menu, it does not grab anything
	gtk_widget_hide(self->model_popover_);
	if (event->button != GDK_BUTTON_SECONDARY) {
		return FALSE;
	}

	// The model under the pointer is the one the menu is about
	GtkTreePath* path = nullptr;
	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), static_cast<gint>(event->x), static_cast<gint>(event->y), &path, nullptr, nullptr, nullptr) == 0) {
		return FALSE;
	}
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, nullptr, FALSE);
	gtk_tree_path_free(path);

	if (!self->busy_) {
		GdkRectangle rect{0, 0, 1, 1};
		gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(widget), static_cast<gint>(event->x), static_cast<gint>(event->y), &rect.x, &rect.y);
		self->show_model_menu(rect);
	}
	return TRUE;
}

// The menu key of the keyboard
gboolean MainWindow::on_tree_popup_menu(GtkWidget* widget, gpointer data) {
	auto* self = static_cast<MainWindow*>(data);
	GtkTreeModel* model = nullptr;
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), &model, &iter) == 0 || self->busy_) {
		return FALSE;
	}

	// Below the selected model
	GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
	GdkRectangle rect;
	gtk_tree_view_get_cell_area(GTK_TREE_VIEW(widget), path, nullptr, &rect);
	gtk_tree_path_free(path);
	rect.width = 1;
	self->show_model_menu(rect);
	return TRUE;
}

void MainWindow::show_model_menu(GdkRectangle rect) {
	gtk_popover_set_pointing_to(GTK_POPOVER(model_popover_), &rect);
	gtk_popover_popup(GTK_POPOVER(model_popover_));
}

void MainWindow::build_model_menu() {
	// Not modal: a modal popover or a menu grabs the pointer, and then the click on the close
	// button of the window only closes the menu
	model_popover_ = gtk_popover_new(tree_view_);
	gtk_popover_set_modal(GTK_POPOVER(model_popover_), FALSE);

	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_set_border_width(GTK_CONTAINER(box), 4);
	gtk_container_add(GTK_CONTAINER(model_popover_), box);

	const auto add_item = [&](const char* label, const char* icon_name, void (*handler)(GtkButton*, gpointer)) {
		// A flat button with the icon in front of the text, like the entries of a menu
		GtkWidget* item = gtk_button_new();
		gtk_button_set_relief(GTK_BUTTON(item), GTK_RELIEF_NONE);
		GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_box_pack_start(GTK_BOX(content), gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
		GtkWidget* text = gtk_label_new(label);
		gtk_label_set_xalign(GTK_LABEL(text), 0.0F);
		gtk_box_pack_start(GTK_BOX(content), text, TRUE, TRUE, 0);
		gtk_container_add(GTK_CONTAINER(item), content);
		// The list keeps the focus, losing it closes the menu
		gtk_widget_set_focus_on_click(item, FALSE);
		g_signal_connect(item, "clicked", G_CALLBACK(handler), this);
		gtk_box_pack_start(GTK_BOX(box), item, FALSE, FALSE, 0);
	};
	add_item(
		S("Rename"), "document-edit-symbolic", +[](GtkButton* /*item*/, gpointer data) {
			auto* self = static_cast<MainWindow*>(data);
			gtk_widget_hide(self->model_popover_);
			self->ask_to_rename_model();
		});
	add_item(
		S("Delete"), "edit-delete-symbolic", +[](GtkButton* /*item*/, gpointer data) {
			auto* self = static_cast<MainWindow*>(data);
			gtk_widget_hide(self->model_popover_);
			self->ask_to_delete_model();
		});
	gtk_widget_show_all(box);

	g_signal_connect(tree_view_, "button-press-event", G_CALLBACK(on_tree_button_press), this);
	g_signal_connect(tree_view_, "popup-menu", G_CALLBACK(on_tree_popup_menu), this);

	// The menu goes away with anything else that happens: the focus leaving the list, a key,
	// another tab
	const auto close_on_event = +[](GtkWidget* /*widget*/, GdkEvent* /*event*/, gpointer data) -> gboolean {
		gtk_widget_hide(static_cast<MainWindow*>(data)->model_popover_);
		return FALSE;
	};
	g_signal_connect(tree_view_, "focus-out-event", G_CALLBACK(close_on_event), this);
	g_signal_connect(tree_view_, "key-press-event", G_CALLBACK(close_on_event), this);
	g_signal_connect(window_, "focus-out-event", G_CALLBACK(close_on_event), this);
	g_signal_connect(
		gtk_builder_get_object(builder_, "notebook"), "switch-page",
		G_CALLBACK(+[](GtkNotebook* /*notebook*/, GtkWidget* /*page*/, guint /*number*/, gpointer data) {
			gtk_widget_hide(static_cast<MainWindow*>(data)->model_popover_);
		}),
		this);
}

gboolean MainWindow::on_about_link(GtkLabel* /*label*/, gchar* uri, gpointer data) {
	auto* self = static_cast<MainWindow*>(data);

	// Open links as the user, not as root
	const char* sudo_user = std::getenv("SUDO_USER");
	if (geteuid() == 0 && sudo_user != nullptr && *sudo_user != '\0') {
		const char* argv[] = {"sudo", "-u", sudo_user, "timeout", "10", "xdg-open", uri, nullptr};
		g_spawn_async(
			nullptr, const_cast<gchar**>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
			nullptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
	} else {
		gtk_show_uri_on_window(GTK_WINDOW(self->window_), uri, GDK_CURRENT_TIME, nullptr);
	}

	// The link is handled
	return TRUE;
}

void MainWindow::error(const std::string& title, const std::string& detail) {
	on_error_(title, detail);
}

void MainWindow::set_busy(bool busy) {
	busy_ = busy;
	if (window_ != nullptr) {
		gtk_widget_set_sensitive(window_, busy ? FALSE : TRUE);
	}
}

void MainWindow::ask_for_model_name() {
	if (busy_) {
		return;
	}

	GtkWidget* dialog = gtk_message_dialog_new(
		GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s",
		S("Please enter a name for the new model, 24 characters max"));
	gtk_window_set_title(GTK_WINDOW(dialog), S("Confirm Model Creation"));
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	GtkWidget* entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(entry), 24);
	// Enter in the field confirms the dialog
	g_signal_connect(
		entry, "activate", G_CALLBACK(+[](GtkWidget* /*entry*/, gpointer dialog_data) { gtk_dialog_response(GTK_DIALOG(dialog_data), GTK_RESPONSE_OK); }),
		dialog);

	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(S("Model name:")), FALSE, FALSE, 5);
	gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 5);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE, 5);
	gtk_widget_show_all(dialog);

	const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	const std::string name = trim(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_OK) {
		add_model(name);
	}
}

void MainWindow::ask_to_rename_model() {
	if (busy_) {
		return;
	}

	GtkTreeModel* model = nullptr;
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view_)), &model, &iter) == 0) {
		return;
	}

	gchar* id = nullptr;
	gchar* label = nullptr;
	gtk_tree_model_get(model, &iter, 0, &id, 2, &label, -1);
	const std::string model_id = id;
	const std::string current = label;
	g_free(id);
	g_free(label);

	GtkWidget* dialog = gtk_message_dialog_new(
		GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s",
		S("Please enter a new name for the model, 24 characters max"));
	gtk_window_set_title(GTK_WINDOW(dialog), S("Rename Model"));
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	GtkWidget* entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(entry), 24);
	gtk_entry_set_text(GTK_ENTRY(entry), current.c_str());
	// Enter in the field confirms the dialog
	g_signal_connect(
		entry, "activate", G_CALLBACK(+[](GtkWidget* /*entry*/, gpointer dialog_data) { gtk_dialog_response(GTK_DIALOG(dialog_data), GTK_RESPONSE_OK); }),
		dialog);

	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(S("Model name:")), FALSE, FALSE, 5);
	gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 5);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE, 5);
	gtk_widget_show_all(dialog);

	const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	std::string name = trim(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(dialog);

	// The list is comma separated for machines
	std::erase(name, ',');
	if (response == GTK_RESPONSE_OK && !name.empty() && name != current) {
		rename_model(model_id, current, name);
	}
}

void MainWindow::ask_to_delete_model() {
	if (busy_) {
		return;
	}

	GtkTreeModel* model = nullptr;
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view_)), &model, &iter) == 0) {
		return;
	}

	gchar* id = nullptr;
	gchar* label = nullptr;
	gtk_tree_model_get(model, &iter, 0, &id, 2, &label, -1);
	const std::string model_id = id;
	const std::string model_label = label;
	const std::string text = std::string("Are you sure you want to delete model ") + id + " (" + label + ")?";
	g_free(id);
	g_free(label);

	GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", text.c_str());
	gtk_window_set_title(GTK_WINDOW(dialog), S("Confirm Model Deletion"));
	const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_OK) {
		delete_model(model_id, model_label);
	}
}

void MainWindow::start(std::function<void()> done) {
	// Listing the models needs the same permission as everything else here
	reload_models(
		[this, done = std::move(done)] {
			if (authorized_ && listed_ && models_.empty() && on_no_models_) {
				on_no_models_();
			} else if (authorized_ && window_ != nullptr) {
				gtk_widget_show_all(window_);
			}
			if (done) {
				done();
			}
		},
		true);
}

std::string MainWindow::session_user_name() {
	return session_user();
}

std::string MainWindow::next_model_name() const {
	// The same name `howdy add` would give it
	long long next = 0;
	if (!models_.empty()) {
		next = std::strtoll(models_.back().id.c_str(), nullptr, 10) + 1;
	}
	return std::string("Model #") + std::to_string(next);
}

void MainWindow::reload_models(std::function<void()> done, bool denied_is_fatal) {
	const auto alive = alive_;
	client_.list(user_, [this, alive, done = std::move(done), denied_is_fatal](const ManagementResult& result) {
		if (!*alive) {
			return;
		}

		models_.clear();
		std::string problem = result.error;
		if (result.ok) {
			try {
				models_ = parse_model_list(result.reply);
			} catch (const std::exception& exc) {
				problem = exc.what();
			}
		}

		listed_ = result.ok && problem.empty();

		gtk_list_store_clear(list_store_);
		for (const auto& model : models_) {
			GtkTreeIter iter;
			gtk_list_store_append(list_store_, &iter);
			gtk_list_store_set(list_store_, &iter, 0, model.id.c_str(), 1, model.created.c_str(), 2, model.label.c_str(), -1);
		}

		// Without the permission to manage the models there is nothing to do here
		if (!result.not_authorized) {
			authorized_ = true;
		}

		if (result.not_authorized && denied_is_fatal) {
			on_fatal_("Authorization failed: " + problem);
		} else if (!problem.empty()) {
			error(S("Failed to load the face models"), problem);
		}
		if (done) {
			done();
		}
	});
}

void MainWindow::add_model(const std::string& name, std::function<void()> done) {
	// The face is captured and the model saved from here: the dialog that guides
	// the user belongs to this window, and the model is saved over the connection
	// that is kept open, so polkit asks for a password once for a while.
	const std::string label = name.empty() ? next_model_name() : name;

	set_busy(true);
	const auto alive = alive_;
	enroller_.enroll(GTK_WINDOW(window_), label, [this, alive, done = std::move(done)](const ModelEnroller::Result& result) {
		if (!*alive) {
			return;
		}
		set_busy(false);

		// The user closed the dialog, there is nothing to report
		if (result.outcome == ModelEnroller::Outcome::Failed) {
			error(S("Error while adding model:"), trim(result.error));
		}
		reload_models(done);
	});
}

void MainWindow::delete_model(const std::string& id, const std::string& expected_label, std::function<void()> done) {
	set_busy(true);
	const auto alive = alive_;
	client_.remove(user_, id, expected_label, [this, alive, done = std::move(done)](const ManagementResult& result) {
		if (!*alive) {
			return;
		}
		set_busy(false);

		if (!result.ok) {
			error(S("Error while deleting model"), result.error);
		}
		reload_models(done);
	});
}

void MainWindow::rename_model(const std::string& id, const std::string& expected_label, const std::string& label, std::function<void()> done) {
	set_busy(true);
	const auto alive = alive_;
	client_.rename(user_, id, expected_label, label, [this, alive, done = std::move(done)](const ManagementResult& result) {
		if (!*alive) {
			return;
		}
		set_busy(false);

		if (!result.ok) {
			error(S("Error while renaming model"), result.error);
		}
		reload_models(done);
	});
}

} // namespace gtkui

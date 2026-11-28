import os
import builtins

from gi.repository import Gtk as gtk
from gi.repository import GLib as glib
from gi.repository import Gdk as gdk

os.environ["GDK_BACKEND"] = "x11"


from .i18n import S
from howdy import enroll_helper  # type: ignore
from howdy.cli import add  # type: ignore
from howdy.model_limits import many_models, many_models_warning  # type: ignore
from collections import namedtuple

Args = namedtuple("args", ["y", "gui", "plain", "user", "arguments"])


def on_error(subtext, error, parent):
	if isinstance(error, enroll_helper.EnrollCancelled):
		return

	error_dialog = gtk.MessageDialog(
		parent=parent,
		flags=gtk.DialogFlags.MODAL,
		type=gtk.MessageType.ERROR,
		buttons=gtk.ButtonsType.CLOSE,
	)

	error_dialog.set_title(S("Howdy Error"))
	error_dialog.set_markup(subtext)
	error_dialog.format_secondary_text(str(error))

	error_dialog.run()
	error_dialog.destroy()


def on_model_add(self, button):
	# Open question dialog
	dialog = gtk.MessageDialog(
		parent=self.window,
		flags=gtk.DialogFlags.MODAL,
		type=gtk.MessageType.QUESTION,
		buttons=gtk.ButtonsType.OK_CANCEL,
	)
	dialog.set_default_response(gtk.ResponseType.OK)
	dialog.set_title(S("Confirm Model Creation"))
	dialog.set_markup(S("Please enter a name for the new model, 24 characters max"))

	# Create the input field
	entry = gtk.Entry()
	entry.set_max_length(24)
	entry.connect("activate", lambda _: dialog.response(gtk.ResponseType.OK))

	# Add a label to ask for a model name
	hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
	hbox.pack_start(gtk.Label(label=S("Model name:")), False, False, 5)
	hbox.pack_start(entry, True, True, 5)

	# Add the box to the dialog
	content_area = dialog.get_content_area()

	# The same warning as `howdy add` gives, where the name is asked for
	if many_models(len(self.listmodel)):
		warning = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=8)
		warning.pack_start(
			gtk.Image.new_from_icon_name("dialog-warning", gtk.IconSize.LARGE_TOOLBAR),
			False,
			False,
			5,
		)
		text = gtk.Label(label=many_models_warning())
		text.set_line_wrap(True)
		text.set_max_width_chars(48)
		text.set_xalign(0.0)
		warning.pack_start(text, True, True, 0)
		content_area.pack_start(warning, False, False, 5)

	content_area.pack_start(hbox, True, True, 5)

	dialog.show_all()

	# Show dialog
	response = dialog.run()

	entered_name = entry.get_text()
	dialog.destroy()

	if response == gtk.ResponseType.OK:
		dialog = enroll_helper.create_enroll_dialog(parent=self.window)

		content = dialog.get_content_area()
		box = gtk.Box(orientation=gtk.Orientation.VERTICAL, spacing=8)
		box.set_margin_top(20)
		box.set_margin_bottom(20)
		box.set_margin_start(20)
		box.set_margin_end(20)

		content.add(box)

		info_label = gtk.Label()
		info_label.set_xalign(0.0)
		info_label.set_line_wrap(True)
		info_label.set_max_width_chars(48)
		info_label.set_markup(
			S(
				"Keep your face inside the camera "
				"view and follow the prompts.\n"
				"This takes about a minute."
			)
		)
		box.pack_start(info_label, False, False, 0)

		guide_label = gtk.Label()
		guide_label.get_style_context().add_class("heading")
		guide_label.set_markup(
			S(
				"<big><span foreground='#2AAAE5'><b>Please look straight into the camera</b></span></big>"
			)
		)
		box.pack_start(guide_label, False, False, 25)

		progress_label = gtk.Label()
		# progress_label.set_xalign(0.0)
		progress_label.get_style_context().add_class("dim-label")
		box.pack_start(progress_label, False, False, 0)

		dialog.show_all()

		# Wait a bit to allow the user to read the dialog
		glib.timeout_add(
			600,
			lambda: execute_add(
				self,
				dialog,
				entered_name,
			),
		)


def execute_add(
	self,
	dialog,
	entered_name,
):
	"""Run `howdy add` on a worker thread; the dialog shows
	the live guidance.
	"""

	builtins.howdy_args = Args(
		y=True, gui=True, plain=True, user=self.active_user, arguments=[entered_name]
	)

	try:
		add.run(dialog)
	except Exception as exc:
		on_error(S("Error while adding model:"), exc, self.window)
	finally:
		dialog.destroy()
		self.load_model_list()
	return False


def on_model_delete(self, button):
	selection = self.treeview.get_selection()
	listmodel, rowlist = selection.get_selected_rows()

	if len(rowlist) == 1:
		tree_iter = listmodel.get_iter(rowlist[0])

		model_id = listmodel.get_value(tree_iter, 0)
		name = listmodel.get_value(tree_iter, 2)
		seconds = listmodel.get_value(tree_iter, 3)

		dialog = gtk.MessageDialog(
			parent=self, flags=gtk.DialogFlags.MODAL, buttons=gtk.ButtonsType.OK_CANCEL
		)
		dialog.set_title(S("Confirm Model Deletion"))
		# The name is text, not markup: "&" or "<" in it would break the dialog
		dialog.set_markup(
			S("Are you sure you want to delete model {} ({})?").format(
				glib.markup_escape_text(model_id), glib.markup_escape_text(name)
			)
		)

		response = dialog.run()
		dialog.destroy()

		builtins.howdy_args = Args(
			y=True, gui=True, plain=True, user=self.active_user, arguments=[model_id]
		)

		if response == gtk.ResponseType.OK:
			try:
				from howdy.cli import remove  # type: ignore

				remove.run(expected_label=name, expected_time=seconds)
			except Exception as exc:
				on_error(S("Error while deleting model"), str(exc), self.window)
			finally:
				self.load_model_list()


def on_model_rename(self, widget=None):
	selection = self.treeview.get_selection()
	listmodel, rowlist = selection.get_selected_rows()

	if len(rowlist) != 1:
		return

	tree_iter = listmodel.get_iter(rowlist[0])
	model_id = listmodel.get_value(tree_iter, 0)
	current = listmodel.get_value(tree_iter, 2)
	seconds = listmodel.get_value(tree_iter, 3)

	dialog = gtk.MessageDialog(
		parent=self.window,
		flags=gtk.DialogFlags.MODAL,
		type=gtk.MessageType.QUESTION,
		buttons=gtk.ButtonsType.OK_CANCEL,
	)
	dialog.set_default_response(gtk.ResponseType.OK)
	dialog.set_title(S("Rename Model"))
	dialog.set_markup(S("Please enter a new name for the model, 24 characters max"))

	# Create the input field with the current name
	entry = gtk.Entry()
	entry.set_max_length(24)
	entry.set_text(current)
	entry.connect("activate", lambda _: dialog.response(gtk.ResponseType.OK))

	hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
	hbox.pack_start(gtk.Label(label=S("Model name:")), False, False, 5)
	hbox.pack_start(entry, True, True, 5)
	dialog.get_content_area().pack_start(hbox, True, True, 5)
	dialog.show_all()

	response = dialog.run()
	# The list is comma separated for machines
	name = entry.get_text().strip().replace(",", "")
	dialog.destroy()

	if response != gtk.ResponseType.OK or not name or name == current:
		return

	builtins.howdy_args = Args(
		y=True,
		gui=True,
		plain=True,
		user=self.active_user,
		arguments=[model_id, name],
	)

	try:
		from howdy.cli import rename  # type: ignore

		rename.run(expected_label=current, expected_time=seconds)
	except Exception as exc:  # noqa: BLE001
		on_error(S("Error while renaming model"), str(exc), self.window)
	finally:
		self.load_model_list()


def on_tree_button_press(self, treeview, event):
	"""Right click on a model: offer to rename or delete it"""
	if event.type != gdk.EventType.BUTTON_PRESS:
		return False

	# Any other click on the list closes the menu, it does not grab anything
	self.model_popover.hide()
	if event.button != 3:
		return False

	# The model under the pointer is the one the menu is about
	found = treeview.get_path_at_pos(int(event.x), int(event.y))
	if found is None:
		return False
	treeview.set_cursor(found[0], None, False)

	x, y = treeview.convert_bin_window_to_widget_coords(int(event.x), int(event.y))
	show_model_menu(self, x, y)
	return True


def on_tree_popup_menu(self, treeview):
	"""The menu key of the keyboard"""
	listmodel, tree_iter = treeview.get_selection().get_selected()
	if tree_iter is None:
		return False

	# Below the selected model
	area = treeview.get_cell_area(listmodel.get_path(tree_iter), None)
	show_model_menu(self, area.x, area.y)
	return True


def show_model_menu(self, x, y):
	rect = gdk.Rectangle()
	rect.x, rect.y, rect.width, rect.height = x, y, 1, 1
	self.model_popover.set_pointing_to(rect)
	self.model_popover.popup()


def on_tree_closes_menu(self, *args):
	"""The menu goes away with anything else that happens: the focus leaving the
	list, a key"""
	self.model_popover.hide()
	return False


def on_model_menu_item(self, item, handler):
	self.model_popover.hide()
	handler(item)

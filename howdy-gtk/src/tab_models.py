import os
import builtins

from gi.repository import Gtk as gtk
from gi.repository import GLib as glib

os.environ["GDK_BACKEND"] = "x11"


from .i18n import S
from howdy import enroll_helper  # type: ignore
from howdy.cli import add  # type: ignore
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
	entry.connect("activate", lambda _: dialog.response(gtk.ResponseType.OK))

	# Add a label to ask for a model name
	hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
	hbox.pack_start(gtk.Label(label=S("Model name:")), False, False, 5)
	hbox.pack_start(entry, True, True, 5)

	# Add the box to the dialog
	content_area = dialog.get_content_area()
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

		dialog = gtk.MessageDialog(
			parent=self, flags=gtk.DialogFlags.MODAL, buttons=gtk.ButtonsType.OK_CANCEL
		)
		dialog.set_title(S("Confirm Model Deletion"))
		dialog.set_markup(
			S(f"Are you sure you want to delete model {model_id} ({name})?")
		)

		response = dialog.run()
		dialog.destroy()

		builtins.howdy_args = Args(
			y=True, gui=True, plain=True, user=self.active_user, arguments=[model_id]
		)

		if response == gtk.ResponseType.OK:
			try:
				from howdy.cli import remove  # type: ignore

				remove.run()
			except Exception as exc:
				on_error(S("Error while deleting model"), str(exc), self.window)
			finally:
				self.load_model_list()

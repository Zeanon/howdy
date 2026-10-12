import subprocess
import enroll
import cairo
import math
import os

from i18n import _
from gi.repository import Gtk as gtk
from gi.repository import Gdk as gdk
from gi.repository import GObject as gObject

os.environ["GDK_BACKEND"] = "x11"

HIGHLIGHT_COLOR = (42, 170, 229) #2AAAE5

def on_user_change(self, select):
	self.active_user = select.get_active_text()
	self.load_model_list()


def on_user_add(self, button):
	# Open question dialog
	dialog = gtk.MessageDialog(
		parent=self,
		flags=gtk.DialogFlags.MODAL,
		type=gtk.MessageType.QUESTION,
		buttons=gtk.ButtonsType.OK_CANCEL
	)
	dialog.set_title(_("Confirm User Creation"))
	dialog.set_markup(
		_("Please enter the username of the user you want to add to Howdy")
	)

	# Create the input field
	entry = gtk.Entry()

	# Add a label to ask for a username
	hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
	hbox.pack_start(
		gtk.Label(label=_("Username:")),
		False,
		False,
		5
	)
	hbox.pack_start(entry, True, True, 5)

	# Add the box to the dialog
	content_area = dialog.get_content_area()
	content_area.pack_start(hbox, True, True, 5)

	dialog.show_all()

	# Show dialog
	response = dialog.run()

	entered_user = entry.get_text()
	dialog.destroy()

	if response == gtk.ResponseType.OK:
		self.userlist.append_text(entered_user)
		self.userlist.set_active(self.userlist.items)

		# Keep compatibility with the existing userlist implementation.
		self.userlist.items += 1

		self.active_user = entered_user
		self.load_model_list()


def on_model_add(self, button):
	if self.userlist.items == 0:
		return

	# Open question dialog
	dialog = gtk.MessageDialog(
		parent=self,
		flags=gtk.DialogFlags.MODAL,
		type=gtk.MessageType.QUESTION,
		buttons=gtk.ButtonsType.OK_CANCEL
	)
	dialog.set_default_response(gtk.ResponseType.OK)
	dialog.set_title(_("Confirm Model Creation"))
	dialog.set_markup(
		_("Please enter a name for the new model, 24 characters max")
	)

	# Create the input field
	entry = gtk.Entry()
	entry.connect("activate", lambda _: dialog.response(gtk.ResponseType.OK))

	# Add a label to ask for a model name
	hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
	hbox.pack_start(
		gtk.Label(label=_("Model name:")),
		False,
		False,
		5
	)
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
		dialog_width = 500
		dialog_height = 545

		# GTK 3 replacement for gtk4compat.PromptWindow
		dialog = gtk.Dialog(
			title=_("Creating Model"),
			parent=self.window,
			flags=gtk.DialogFlags.MODAL
		)
		dialog.connect(
			"draw",
			lambda widget, ctx: draw_background(widget, ctx)
		)

		dialog.set_type_hint(gdk.WindowTypeHint.NOTIFICATION)
		dialog.set_default_size(dialog_width, dialog_height)
		dialog.set_app_paintable(True)
		dialog.set_focus_on_map(False)
		dialog.set_keep_above(True)
		dialog.set_resizable(False)
		dialog.set_can_focus(False)
		dialog.set_decorated(False)
		dialog.set_modal(True)
		
		screen = gdk.Screen.get_default()
		dialog.move((screen.get_width() / 2) - (dialog_width / 2), 0)

		content_area = dialog.get_content_area()

		# Main content
		box = gtk.Box(
			orientation=gtk.Orientation.VERTICAL,
			spacing=8
		)
		box.set_margin_top(20)
		box.set_margin_bottom(20)
		box.set_margin_start(20)
		box.set_margin_end(20)

		content_area.add(box)

		info_label = gtk.Label()
		info_label.set_xalign(0.0)
		info_label.set_line_wrap(True)
		info_label.set_max_width_chars(48)
		info_label.set_markup(_("Keep your face inside the camera "
								 "view and follow the prompts.\n" \
								 "This takes about a minute."))
		box.pack_start(info_label, False, False, 0)

		# box.pack_start(inner_box, True, True, 0)

		guide_label = gtk.Label()
		# GTK 3 replacement for add_css_class("heading")
		guide_label.get_style_context().add_class("heading")
		guide_label.set_markup(_(f"<big><span foreground='#2AAAE5'><b>Look straight at the camera</b></span></big>"))
		box.pack_start(guide_label, False, False, 25)

		progress_label = gtk.Label(label="")
		# progress_label.set_xalign(0.0)

		# GTK 3 replacement for add_css_class("dim-label")
		progress_label.get_style_context().add_class("dim-label")

		box.pack_start(progress_label, False, False, 0)

		dialog.show_all()

		# Wait a bit to allow the user to read the dialog
		gObject.timeout_add(
			600,
			lambda: execute_add(
				self,
				dialog,
				entered_name,
				guide_label,
				progress_label
			)
		)

		def draw_background(widget, ctx):
			radius = 18
			border = 2
			half = border / 2

			# Fensterfläche transparent löschen
			ctx.set_operator(cairo.OPERATOR_SOURCE)
			ctx.set_source_rgba(0, 0, 0, 0)
			ctx.paint()

			ctx.set_operator(cairo.OPERATOR_OVER)
			ctx.new_path()

			# Linke Kante, ohne obere Kante
			ctx.move_to(half, 0)
			ctx.line_to(half, dialog_height - radius - half)

			# Linke untere Rundung
			ctx.arc_negative(
				radius + half,
				dialog_height - radius - half,
				radius,
				math.pi,
				math.pi / 2
			)

			# Untere Kante
			ctx.line_to(dialog_width - radius - half, dialog_height - half)

			# Rechte untere Rundung
			ctx.arc_negative(
				dialog_width - radius - half,
				dialog_height - radius - half,
				radius,
				math.pi / 2,
				0
			)

			# Rechte Kante, ohne obere Kante
			ctx.line_to(dialog_width - half, 0)

			# Theme-Hintergrundfarbe auslesen
			style = widget.get_style_context()
			found, bg_color = style.lookup_color("theme_bg_color")
			# bg_color = style.get_background_color(gtk.StateFlags.NORMAL)

			if found:
				ctx.set_source_rgba(bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha)
			else:
				# Fallback, falls die Farbe nicht verfügbar ist
				ctx.set_source_rgb(0, 0, 0)
			ctx.fill_preserve()

			ctx.set_line_width(border)
			ctx.set_line_join(cairo.LINE_JOIN_ROUND)
			ctx.set_source_rgb(HIGHLIGHT_COLOR[0]/255, HIGHLIGHT_COLOR[1]/255, HIGHLIGHT_COLOR[2]/255)

			ctx.stroke()


def execute_add(
	box,
	dialog,
	entered_name,
	guide_label=None,
	progress_label=None
):
	"""Run `howdy add` on a worker thread; the dialog shows
	the live guidance.
	"""

	def on_guide(key):
		text = enroll.guide_text(key)

		if guide_label is not None and text:
			guide_label.set_markup(_(f"<big><span foreground='#2AAAE5'><b>{text}</b></span></big>"))

		return False

	def on_progress(count, total):
		if progress_label is not None:
			progress_label.set_markup(
				enroll.progress_text(count, total)
			)

		return False

	def on_done(status, output):
		dialog.destroy()

		if status != 0:
			error_dialog = gtk.MessageDialog(
				parent=box.window,
				flags=gtk.DialogFlags.MODAL,
				type=gtk.MessageType.ERROR,
				buttons=gtk.ButtonsType.CLOSE
			)

			error_dialog.set_title(_("Howdy Error"))
			error_dialog.set_markup(
				_("Error while adding model, error code {}:").format(status)
			)
			error_dialog.format_secondary_text(output)

			error_dialog.run()
			error_dialog.destroy()

		box.load_model_list()
		return False

	enroll.run_add(
		[
			"howdy",
			"--gui",
			"-y",
			"-U",
			box.active_user,
			"add",
			entered_name
		],
		on_guide,
		on_progress,
		on_done
	)

	return False


def on_model_delete(self, button):
	selection = self.treeview.get_selection()
	listmodel, rowlist = selection.get_selected_rows()

	if len(rowlist) == 1:
		tree_iter = listmodel.get_iter(rowlist[0])

		model_id = listmodel.get_value(tree_iter, 0)
		name = listmodel.get_value(tree_iter, 2)

		dialog = gtk.MessageDialog(
			parent=self,
			flags=gtk.DialogFlags.MODAL,
			buttons=gtk.ButtonsType.OK_CANCEL
		)
		dialog.set_title(_("Confirm Model Deletion"))
		dialog.set_markup(
			_("Are you sure you want to delete model {id} ({name})?")
			.format(id=model_id, name=name)
		)

		response = dialog.run()
		dialog.destroy()

		if response == gtk.ResponseType.OK:
			status, output = subprocess.getstatusoutput(
				"howdy remove {} -y -U {}".format(
					model_id,
					self.active_user
				)
			)

			if status != 0:
				error_dialog = gtk.MessageDialog(
					parent=self,
					flags=gtk.DialogFlags.MODAL,
					type=gtk.MessageType.ERROR,
					buttons=gtk.ButtonsType.CLOSE
				)

				error_dialog.set_title(_("Howdy Error"))
				error_dialog.set_markup(
					_("Error while deleting model, error code {}:")
					.format(status)
				)
				error_dialog.format_secondary_text(output)

				error_dialog.run()
				error_dialog.destroy()

			self.load_model_list()

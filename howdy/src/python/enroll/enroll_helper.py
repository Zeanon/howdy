# The exit status of `howdy add` when the user has cancelled, as the C++ program has it
EXIT_CANCELLED = 3


class EnrollCancelled(Exception):
	# Exception raised when enrollment was manually cancelled
	def __init__(self, *args: object):
		super().__init__(args)


def create_enroll_dialog(parent):
	import os
	import gi
	import math
	import cairo

	gi.require_version("Gtk", "3.0")
	from gi.repository import Gtk as gtk
	from gi.repository import Gdk as gdk

	os.environ["GDK_BACKEND"] = "x11"

	dialog_width = 500
	dialog_height = 605

	HIGHLIGHT_COLOR = (42, 170, 229)  # 2AAAE5

	def draw_background(widget, ctx):
		radius = 18
		border = 2
		half = border / 2

		# dialog.get_window().set_cursor(gdk.Cursor(gdk.CursorType.TARGET)) #TODO change?

		# Make window transparent
		ctx.set_operator(cairo.OPERATOR_SOURCE)
		ctx.set_source_rgba(0, 0, 0, 0)
		ctx.paint()

		ctx.set_operator(cairo.OPERATOR_OVER)
		ctx.new_path()

		# left edge
		ctx.move_to(half, 0)
		ctx.line_to(half, dialog_height - radius - half)

		# lower left corner
		ctx.arc_negative(
			radius + half, dialog_height - radius - half, radius, math.pi, math.pi / 2
		)

		# lower edge
		ctx.line_to(dialog_width - radius - half, dialog_height - half)

		# lower right corner
		ctx.arc_negative(
			dialog_width - radius - half,
			dialog_height - radius - half,
			radius,
			math.pi / 2,
			0,
		)

		# right edge
		ctx.line_to(dialog_width - half, 0)

		# get the themes background color
		style = widget.get_style_context()
		found, bg_color = style.lookup_color("theme_bg_color")
		# bg_color = style.get_background_color(gtk.StateFlags.NORMAL)

		if found:
			ctx.set_source_rgba(
				bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha
			)
		else:
			# Fallback
			ctx.set_source_rgb(0, 0, 0)
		ctx.fill_preserve()

		ctx.set_line_width(border)
		ctx.set_line_join(cairo.LINE_JOIN_ROUND)
		ctx.set_source_rgb(
			HIGHLIGHT_COLOR[0] / 255, HIGHLIGHT_COLOR[1] / 255, HIGHLIGHT_COLOR[2] / 255
		)

		ctx.stroke()

	dialog = gtk.Dialog(
		title="Howdy – Create Model", parent=parent, flags=gtk.DialogFlags.MODAL
	)
	dialog.connect("draw", draw_background)

	dialog.set_type_hint(gdk.WindowTypeHint.NOTIFICATION)
	dialog.set_default_size(dialog_width, dialog_height)
	dialog.set_app_paintable(True)
	dialog.set_focus_on_map(False)
	dialog.set_keep_above(True)
	dialog.set_resizable(False)
	dialog.set_can_focus(False)
	dialog.set_decorated(False)
	dialog.set_modal(True)

	visual = gdk.Screen.get_default().get_rgba_visual()
	if visual is not None:
		dialog.set_visual(visual)

	display = gdk.Display.get_default()
	monitor = display.get_primary_monitor()
	if monitor is None:
		monitor = display.get_monitor(0)

	geometry = monitor.get_geometry()
	dialog.move(
		geometry.x + (geometry.width - dialog_width) // 2,
		geometry.y,
	)

	return dialog

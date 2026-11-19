# Shows a floating window when authenticating.
import gi
import os
import math
import time
import cairo
import syslog
import traceback

from . import paths_factory
from dbus_next.glib import MessageBus
from dbus_next import BusType, Message, MessageType

from .i18n import S

os.environ["GDK_BACKEND"] = "x11"

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")

from gi.repository import Gtk as gtk
from gi.repository import Gdk as gdk
from gi.repository import GLib as glib


WINDOW_WIDTH = 400
WINDOW_HEIGHT = 100
HIGHLIGHT_COLOR = (42, 170, 229)


BUS_NAME = "@authentication_bus@"
POPUP_PATH = "@popup_path@"
POPUP_INTERFACE = "@popup_interface@"

SCREENSAVER_PATH = "/org/freedesktop/ScreenSaver"
SCREENSAVER_INTERFACE = "org.freedesktop.ScreenSaver"


window = None
unlock_timeout_id = None


class StickyWindow(gtk.Window):
	def __init__(self, message=S("Loading...")):
		super().__init__()

		self.deadline = time.monotonic() + 2

		self.set_title(S("Howdy Authentication"))
		self.message = message
		self.subtext = ""

		logo_path = paths_factory.logo_popup_path()
		self.logo_surface = cairo.ImageSurface.create_from_png(logo_path)
		self.logo_ratio = float(WINDOW_HEIGHT - 25) / float(
			self.logo_surface.get_height()
		)

		self.stick()
		self.set_type_hint(gdk.WindowTypeHint.NOTIFICATION)
		self.set_default_size(WINDOW_WIDTH, WINDOW_HEIGHT)
		self.set_gravity(gdk.Gravity.STATIC)
		self.set_skip_taskbar_hint(True)
		self.set_skip_pager_hint(True)
		self.set_app_paintable(True)
		self.set_accept_focus(False)
		self.set_focus_on_map(False)
		self.set_can_default(False)
		self.set_keep_above(True)
		self.set_resizable(False)
		self.set_can_focus(False)
		self.set_decorated(False)
		self.set_focus(None)

		self.connect("draw", self.draw)
		self.connect("destroy", self.on_destroy)

		darea = gtk.DrawingArea()
		darea.set_size_request(WINDOW_WIDTH, WINDOW_HEIGHT)
		self.add(darea)

		visual = gdk.Screen.get_default().get_rgba_visual()
		if visual is not None:
			self.set_visual(visual)

		display = gdk.Display.get_default()
		monitor = display.get_primary_monitor()
		if monitor is None:
			monitor = display.get_monitor(0)

		geometry = monitor.get_geometry()
		self.move(
			geometry.x + (geometry.width - WINDOW_WIDTH) // 2,
			geometry.y,
		)

		glib.timeout_add(100, self.check_timeout)
		self.show_all()

	def check_timeout(self):
		if time.monotonic() > self.deadline:
			self.destroy()
			return False
		return True

	def on_destroy(self, *_):
		global window
		if window is self:
			window = None

	def set_message(self, message):
		self.deadline = time.monotonic() + 1
		self.message = str(message)
		self.queue_draw()

	def set_subtext(self, subtext):
		self.deadline = time.monotonic() + 1
		self.subtext = str(subtext)
		self.queue_draw()

	def draw(self, widget, ctx):
		"""Draw the UI."""
		radius = 17
		border = 2
		half = border / 2

		ctx.set_operator(cairo.OPERATOR_SOURCE)
		ctx.set_source_rgba(0, 0, 0, 0)
		ctx.paint()

		ctx.set_operator(cairo.OPERATOR_OVER)
		ctx.new_path()

		ctx.move_to(half, 0)
		ctx.line_to(half, WINDOW_HEIGHT - radius - half)

		ctx.arc_negative(
			radius + half,
			WINDOW_HEIGHT - radius - half,
			radius,
			math.pi,
			math.pi / 2,
		)

		ctx.line_to(
			WINDOW_WIDTH - radius - half,
			WINDOW_HEIGHT - half,
		)

		ctx.arc_negative(
			WINDOW_WIDTH - radius - half,
			WINDOW_HEIGHT - radius - half,
			radius,
			math.pi / 2,
			0,
		)

		ctx.line_to(WINDOW_WIDTH - half, 0)

		ctx.set_source_rgba(0, 0, 0, 1)
		ctx.fill_preserve()

		ctx.set_line_width(border)
		ctx.set_line_join(cairo.LINE_JOIN_ROUND)
		ctx.set_source_rgb(
			HIGHLIGHT_COLOR[0] / 255,
			HIGHLIGHT_COLOR[1] / 255,
			HIGHLIGHT_COLOR[2] / 255,
		)
		ctx.stroke()

		ctx.translate(15, 10)
		ctx.scale(self.logo_ratio, self.logo_ratio)
		ctx.set_source_surface(self.logo_surface)
		ctx.paint()

		if self.subtext:
			ctx.move_to(380, 145)
		else:
			ctx.move_to(380, 175)

		ctx.set_source_rgba(255, 255, 255, 0.9)
		ctx.set_font_size(80)
		ctx.select_font_face(
			"Arial",
			cairo.FONT_SLANT_NORMAL,
			cairo.FONT_WEIGHT_NORMAL,
		)
		ctx.show_text(self.message)

		if self.subtext:
			ctx.move_to(380, 210)
			ctx.set_source_rgba(230, 230, 230, 0.8)
			ctx.set_font_size(40)
			ctx.select_font_face(
				"Arial",
				cairo.FONT_SLANT_NORMAL,
				cairo.FONT_WEIGHT_NORMAL,
			)
			ctx.show_text(self.subtext)


def on_screen_state_changed(locked: bool):
	global unlock_timeout_id

	if unlock_timeout_id is not None:
		glib.source_remove(unlock_timeout_id)
		unlock_timeout_id = None

	if not locked:
		unlock_timeout_id = glib.timeout_add(500, restart_popup_service)


def restart_popup_service():
	global unlock_timeout_id

	unlock_timeout_id = None

	syslog.syslog(syslog.LOG_INFO, "Screen unlocked; restarting authentication popup")

	gtk.main_quit()
	return False


def run():
	if not gtk.init_check():
		raise RuntimeError("GTK initialization failed: no accessible display")

	terminate = False

	# Normal D-Bus to listen to signals from Howdy
	bus = MessageBus(
		bus_type=BusType.SYSTEM,
	).connect_sync()

	def terminate_service():
		nonlocal terminate
		terminate = True
		syslog.syslog(syslog.LOG_ERR, "Terminating Howdy popup service")
		gtk.main_quit()
		return False

	def handle_message(msg: Message):
		if (
			msg.message_type != MessageType.SIGNAL
			or msg.path != POPUP_PATH
			or msg.interface != POPUP_INTERFACE
			or len(msg.body) < 1
			or int(msg.body[0]) != os.getuid()
		):
			return False

		if msg.member == "Show" and msg.signature == "us":
			global window
			if window is None:
				try:
					window = StickyWindow(msg.body[1])
				except Exception:
					syslog.syslog(
						syslog.LOG_ERR, "Failed to initialize authentication popup"
					)
					syslog.syslog(syslog.LOG_ERR, traceback.format_exc())
					glib.idle_add(terminate_service)
				return True
		elif msg.member == "UpdateMessage" and msg.signature == "us":
			if window is not None:
				window.set_message(msg.body[1])
				return True
		elif msg.member == "UpdateSubtext" and msg.signature == "us":
			if window is not None:
				window.set_subtext(msg.body[1])
				return True
		elif msg.member == "Close" and msg.signature == "u":
			if window is not None:
				window.destroy()
				return True
		return False

	bus.add_message_handler(handle_message)
	bus.call_sync(
		Message(
			destination="org.freedesktop.DBus",
			path="/org/freedesktop/DBus",
			interface="org.freedesktop.DBus",
			member="AddMatch",
			signature="s",
			body=[
				f"type='signal',"
				f"sender='{BUS_NAME}',"
				f"path='{POPUP_PATH}',"
				f"interface='{POPUP_INTERFACE}'"
			],
		)
	)

	# Listen to the screensaver to re-init gtk on unlock
	screensaver_bus = MessageBus(
		bus_type=BusType.SESSION,
	).connect_sync()

	def handle_screensaver_message(msg: Message):
		if (
			msg.message_type != MessageType.SIGNAL
			or msg.path != SCREENSAVER_PATH
			or msg.interface != SCREENSAVER_INTERFACE
			or msg.member != "ActiveChanged"
			or msg.signature != "b"
			or len(msg.body) != 1
		):
			return False

		on_screen_state_changed(msg.body[0])
		return True

	screensaver_bus.add_message_handler(handle_screensaver_message)
	screensaver_bus.call_sync(
		Message(
			destination="org.freedesktop.DBus",
			path="/org/freedesktop/DBus",
			interface="org.freedesktop.DBus",
			member="AddMatch",
			signature="s",
			body=[
				f"type='signal',"
				f"path='{SCREENSAVER_PATH}',"
				f"interface='{SCREENSAVER_INTERFACE}',"
				f"member='ActiveChanged'"
			],
		)
	)

	syslog.syslog(syslog.LOG_INFO, "Howdy popup service is ready")

	gtk.main()
	if terminate:
		raise SystemExit(1)


if __name__ == "__main__":
	while True:
		run()

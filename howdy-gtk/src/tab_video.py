import configparser

from .i18n import S
from . import paths_factory

from gi.repository import Gtk as gtk
from gi.repository import GLib as glib
from gi.repository import GdkPixbuf as gdkPixbuf

MAX_HEIGHT = 300
MAX_WIDTH = 300


def on_page_switch(self, notebook, page, page_num):
	if page_num == 1:
		try:
			self.config = configparser.ConfigParser()
			self.config.read(paths_factory.config_file_path())
		except Exception:
			print(S("Can't open camera"))

		path = self.config.get("video", "device_path")

		try:
			# if not self.cv2:
			import cv2

			self.cv2 = cv2
		except Exception:
			print(S("Can't import OpenCV2"))

		try:
			self.capture = cv2.VideoCapture(path, cv2.CAP_V4L)
		except Exception:
			print(S("Can't open camera"))

		self.original_brightness = None
		self.brightness = self.config.getint("video", "brightness", fallback=None)
		if self.brightness is not None:
			self.original_brightness = self.capture.get(cv2.CAP_PROP_BRIGHTNESS)
			self.capture.set(cv2.CAP_PROP_BRIGHTNESS, self.brightness)

		opencvbox = self.builder.get_object("opencvbox")

		# Give the video preview a black background
		css = gtk.CssProvider()
		css.load_from_data(b"* { background-color: black; }")
		opencvbox.get_style_context().add_provider(
			css, gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
		)

		height = self.capture.get(self.cv2.CAP_PROP_FRAME_HEIGHT) or 1
		width = self.capture.get(self.cv2.CAP_PROP_FRAME_WIDTH) or 1

		self.scaling_factor = (MAX_HEIGHT / height) or 1

		if width * self.scaling_factor > MAX_WIDTH:
			self.scaling_factor = (MAX_WIDTH / width) or 1

		config_height = self.config.getfloat("video", "max_height", fallback=320.0)
		config_scaling = (config_height / height) or 1

		self.builder.get_object("videoid").set_text(path.split("/")[-1])
		self.builder.get_object("videores").set_text(
			str(int(width)) + "x" + str(int(height))
		)
		self.builder.get_object("videoresused").set_text(
			str(int(width * config_scaling)) + "x" + str(int(height * config_scaling))
		)
		self.builder.get_object("videorecorder").set_text(
			self.config.get("video", "recording_plugin", fallback=S("Unknown"))
		)

		glib.timeout_add(10, self.capture_frame)

	else:
		self.release()


def __del__(self):
	"""
	Frees resources when destroyed
	"""
	if self is not None:
		try:
			self.release()
		except AttributeError:
			pass


def release(self):
	"""
	Release cameras
	"""
	if self is not None and self.capture is not None:
		if self.original_brightness is not None:
			self.capture.set(self.cv2.CAP_PROP_BRIGHTNESS, self.original_brightness)
		self.capture.release()
		self.capture = None


def capture_frame(self):
	if self.capture is None:
		return

	_, frame = self.capture.read()

	frame = self.cv2.flip(frame, 1)
	frame = self.cv2.resize(
		frame,
		None,
		fx=self.scaling_factor,
		fy=self.scaling_factor,
		interpolation=self.cv2.INTER_AREA,
	)

	_, buffer = self.cv2.imencode(".png", frame)

	loader = gdkPixbuf.PixbufLoader()
	loader.write(buffer)
	loader.close()
	buffer = loader.get_pixbuf()

	self.opencvimage.set_from_pixbuf(buffer)

	glib.timeout_add(20, self.capture_frame)

# Class that simulates the functionality of opencv so howdy can use v4l2 devices seamlessly

# Import required modules. lib4l-dev package is also required.
import sys
import fcntl
import numpy

from . import v4l2
from cv2 import cvtColor, COLOR_GRAY2BGR, CAP_PROP_FRAME_WIDTH, CAP_PROP_FRAME_HEIGHT

from ..i18n import S

try:
	from pyv4l2.frame import Frame
except ImportError:
	print(S("Missing pyv4l2 module, please run:"))
	print(" pip3 install pyv4l2\n")
	sys.exit(13)


class pyv4l2_reader:
	"""This class was created to look as similar to the openCV features used in Howdy as possible for overall code cleanliness."""

	# Init
	def __init__(self, device_name, device_format):
		self.device_name = device_name
		self.device_format = device_format
		self.height = 0
		self.width = 0
		self.probe()
		self.frame = ""

	def set(self, prop, setting):
		"""Setter method for height and width"""
		if prop == CAP_PROP_FRAME_WIDTH:
			self.width = setting
		elif prop == CAP_PROP_FRAME_HEIGHT:
			self.height = setting

	def get(self, prop):
		"""Getter method for height and width"""
		if prop == CAP_PROP_FRAME_WIDTH:
			return self.width
		elif prop == CAP_PROP_FRAME_HEIGHT:
			return self.height

	def probe(self):
		"""Probe the video device to get height and width info"""

		height = width = 0
		try:
			with open(self.device_name) as vd:
				fmt = v4l2.v4l2_format()
				fmt.type = v4l2.V4L2_BUF_TYPE_VIDEO_CAPTURE
				# Raises OSError if it fails
				fcntl.ioctl(vd, v4l2.VIDIOC_G_FMT, fmt)
				height = fmt.fmt.pix.height
				width = fmt.fmt.pix.width
		except OSError:
			pass

		if not width or not height:
			# Could not determine the resolution from ioctl call. Reverting to slower ffprobe method
			from .ffmpeg_reader import ffprobe_size
			from .video_capture import NoDeviceException

			width, height = ffprobe_size(self.device_name)
			if not width or not height:
				raise NoDeviceException(
					S("Could not determine the resolution of the camera through ffmpeg")
				)

		# The size of the frames the device delivers, whatever set() asks for later
		self.frame_shape = (int(height), int(width))

		if self.get(CAP_PROP_FRAME_HEIGHT) == 0:
			self.set(CAP_PROP_FRAME_HEIGHT, int(height))

		if self.get(CAP_PROP_FRAME_WIDTH) == 0:
			self.set(CAP_PROP_FRAME_WIDTH, int(width))

	def record(self):
		"""Start recording"""
		self.frame = Frame(self.device_name)

	def grab(self):
		"""Read a single frame from the IR camera."""
		self.read()

	def read(self):
		"""Read a single frame from the IR camera."""

		if not self.frame:
			self.record()

		# Grab a raw frame from the camera
		frame_data = self.frame.get_frame()

		# Convert the raw frame_date to a numpy array
		img = numpy.frombuffer(frame_data, numpy.uint8)

		# One byte per pixel, a grayscale image
		if img.size != self.frame_shape[0] * self.frame_shape[1]:
			from .video_capture import NoDeviceException

			raise NoDeviceException(
				S(
					"Failed to read camera specified in the 'device_path' config option, aborting"
				)
			)

		# Return a single frame of video, as BGR like the other readers do
		return 1, cvtColor(img.reshape(self.frame_shape), COLOR_GRAY2BGR)

	def release(self):
		"""Empty our array. If we had a hold on the camera, we would give it back here."""
		self.video = ()
		self.num_frames_read = 0
		if self.frame:
			self.frame.close()

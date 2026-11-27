# Class that simulates the functionality of opencv so howdy can use ffmpeg seamlessly

# Import required modules
import re
import sys
import numpy

from subprocess import Popen, PIPE, TimeoutExpired
from cv2 import CAP_PROP_FRAME_WIDTH
from cv2 import CAP_PROP_FRAME_HEIGHT

from ..i18n import S
from ..exit_codes import CompareResult
from .video_capture import NoDeviceException

try:
	import ffmpeg
except ImportError:
	print(S("Missing ffmpeg module, please run:"))
	print(" pip3 install ffmpeg-python\n")
	sys.exit(CompareResult.Abort)


# How long ffmpeg may take: a camera that delivers nothing would hold the daemon for good
PROCESS_TIMEOUT = 10


def communicate(process):
	"""The output of a program, which is ended if it does not finish in time"""
	try:
		return process.communicate(timeout=PROCESS_TIMEOUT)
	except TimeoutExpired:
		process.kill()
		process.communicate()
		raise NoDeviceException(
			"ffmpeg did not finish in time, is the camera delivering a picture?"
		)


def ffprobe_size(device_path):
	"""(width, height) of the device as ffprobe reports them, as texts, empty if it
	does not"""
	args = [
		"ffprobe",
		"-v",
		"error",
		"-select_streams",
		"v:0",
		"-show_entries",
		"stream=width,height",
		"-of",
		"csv=p=0:s=x",
		device_path,
	]
	out, _ = communicate(Popen(args, stdout=PIPE, stderr=PIPE))
	size = re.search(r"(\d+)x(\d+)", out.decode("utf-8"))
	return size.groups() if size else ("", "")


class ffmpeg_reader:
	"""This class was created to look as similar to the openCV features used in Howdy as possible for overall code cleanliness."""

	def __init__(self, device_path, device_format, numframes=10):
		self.device_path = device_path
		self.device_format = device_format
		self.numframes = numframes
		self.video = ()
		self.num_frames_read = 0
		self.height = 0
		self.width = 0
		self.init_camera = True

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

		# Running this command on ffmpeg unfortunately returns with an exit code of 1, which is silly.
		# Returns an error code of 1 and this text:  "/dev/video2: Immediate exit requested"
		args = [
			"ffmpeg",
			"-f",
			self.device_format,
			"-list_formats",
			"all",
			"-i",
			self.device_path,
		]
		process = Popen(args, stdout=PIPE, stderr=PIPE)
		out, err = communicate(process)
		return_code = process.poll()

		# Worst case scenario, err will equal en empty byte string, b'', so probe will get set to [] here.
		regex = re.compile(r"\s\d{3,4}x\d{3,4}")
		probe = regex.findall(str(err.decode("utf-8")))

		if return_code != 1 or len(probe) < 1:
			# Could not determine the resolution from ffmpeg call. Reverting to ffmpeg.probe()
			width, height = ffprobe_size(self.device_path)
		else:
			(width, height) = [x.strip() for x in probe[0].split("x")]

		if width.isdigit() and height.isdigit() and int(width) and int(height):
			self.fill_size(int(width), int(height))

		if self.get(CAP_PROP_FRAME_WIDTH) <= 0 or self.get(CAP_PROP_FRAME_HEIGHT) <= 0:
			raise NoDeviceException(
				S("Could not determine the resolution of the camera through ffmpeg")
			)

	def fill_size(self, width, height):
		"""Set what was not set from the size of the camera. The picture is scaled to
		what was set, a size that was not set keeps its shape."""
		wanted_width = self.get(CAP_PROP_FRAME_WIDTH)
		wanted_height = self.get(CAP_PROP_FRAME_HEIGHT)
		if wanted_width == 0 and wanted_height == 0:
			self.set(CAP_PROP_FRAME_WIDTH, width)
			self.set(CAP_PROP_FRAME_HEIGHT, height)
		elif wanted_width == 0:
			scaled = max(1, round(width * wanted_height / height))
			self.set(CAP_PROP_FRAME_WIDTH, scaled)
		elif wanted_height == 0:
			scaled = max(1, round(height * wanted_width / width))
			self.set(CAP_PROP_FRAME_HEIGHT, scaled)

	def record(self, numframes):
		"""Record a video, saving it to self.video array for processing later"""

		# Eensure we have set our width and height before we record, otherwise our numpy call will fail
		if self.get(CAP_PROP_FRAME_WIDTH) == 0 or self.get(CAP_PROP_FRAME_HEIGHT) == 0:
			self.probe()

		# Ensure num_frames_read is reset to 0
		self.num_frames_read = 0

		# Record a predetermined amount of frames from the camera
		process = (
			ffmpeg.input(self.device_path, format=self.device_format)
			.output(
				"pipe:",
				format="rawvideo",
				pix_fmt="rgb24",
				vframes=numframes,
				# The frames are cut by this size
				vf=f"scale={int(self.width)}:{int(self.height)}",
			)
			.run_async(pipe_stdout=True, pipe_stderr=True)
		)
		stream, err = communicate(process)
		if process.returncode != 0:
			raise NoDeviceException(f"ffmpeg failed: {err.decode('utf-8', 'replace')}")
		if len(stream) < self.width * self.height * 3:
			raise NoDeviceException(
				S(
					"Failed to read camera specified in the 'device_path' config option, aborting"
				)
			)
		self.video = numpy.frombuffer(stream, numpy.uint8).reshape(
			[-1, self.height, self.width, 3]
		)

	def read(self):
		"""Read a single frame from the self.video array. Will record a video if array is empty."""

		# First time we are called, we want to initialize the camera by probing it, to ensure we have height/width
		# and then take numframes of video to fill the buffer for faster recognition.
		if self.init_camera:
			self.init_camera = False
			self.video = ()
			self.record(self.numframes)
			return 1, self.video[0]

		# If we are called and self.video is empty, we should record self.numframes to fill the video buffer
		if isinstance(self.video, tuple):
			self.record(self.numframes)

		# If we've read max frames, but still are being requested to read more, we simply record another batch.
		# Note, the video array is 0 based, so if numframes is 10, we must subtract 1 or run into an array index
		# error.
		if self.num_frames_read >= (self.numframes - 1):
			self.record(self.numframes)

		# Add one to num_frames_read. If we were at 0, that's fine as frame 0 is almost 100% going to be black
		# as the IR lights aren't fully active yet anyways. Saves us one iteration in the while loop in add/compare.py.
		self.num_frames_read += 1

		# ffmpeg may deliver fewer frames than it was asked for
		if self.num_frames_read >= len(self.video):
			raise NoDeviceException(
				S(
					"Failed to read camera specified in the 'device_path' config option, aborting"
				)
			)

		# Return a single frame of video
		return 1, self.video[self.num_frames_read]

	def release(self):
		"""Empty our array. If we had a hold on the camera, we would give it back here."""
		self.video = ()
		self.num_frames_read = 0

	def grab(self):
		"""Redirect grab() to read() for compatibility"""
		self.read()

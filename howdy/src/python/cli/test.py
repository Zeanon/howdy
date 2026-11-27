# Show a window with the video stream and testing information

# Import required modules
import os
import cv2
import sys
import dlib
import time
import threading
import asyncio
import builtins
import configparser

import numpy as np

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType

from ..i18n import S  # type: ignore
from .. import paths_factory  # type: ignore
from .. import anti_spoof_core  # type: ignore
from ..exit_codes import CompareResult  # type: ignore
from ..rotation import fixed_rotation, rotate_frame  # type: ignore
from ..recorders.video_capture import VideoCapture, NoDeviceException

MANAGEMENT_BUS = "@management_bus@"
MANAGEMENT_PATH = "@management_path@"
MANAGEMENT_INTERFACE = "@management_interface@"

# The window of OpenCV (Qt) is slow on a native Wayland window, the video stutters, and smooth when
# it runs through XWayland. Like the other windows of Howdy (GDK_BACKEND) it always uses X.
os.environ["QT_QPA_PLATFORM"] = "xcb"

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

if config.get("video", "recording_plugin", fallback="opencv") != "opencv":
	print(
		S(
			"Howdy has been configured to use a recorder which doesn't support the test command yet, aborting"
		)
	)
	sys.exit(CompareResult.InvalidDevice)

# The models are not read here, the management daemon compares the faces with them. It only does
# that for root: a program that may ask often enough could work out the models from the answers.
if os.geteuid() != 0:
	print(S("Please run this command as root:\n\n\tsudo howdy test"))
	sys.exit(1)

user = builtins.howdy_args.user
use_openvino = config.getboolean("face_authentication", "use_openvino", fallback=False)
metric = "dot" if use_openvino else "euclidean"


# The video of the window is shown this much larger than the frames of the camera
DISPLAY_SCALE = 1.5


def px(*values):
	"""The overlay (text, bars, ellipses) is drawn on the enlarged video, not on the frame of the
	camera before it is enlarged: drawn small and then stretched, the text is blurry. The
	positions and sizes in the code are the ones of the frame, this turns them into pixels of
	the window."""
	scaled = tuple(round(value * DISPLAY_SCALE) for value in values)
	return scaled if len(scaled) > 1 else scaled[0]


FONT_SCALE_SMALL = 0.3 * DISPLAY_SCALE
FONT_SCALE_NORMAL = 0.5 * DISPLAY_SCALE

# The shortest time in seconds between two rounds of comparisons
COMPARE_INTERVAL = 0.1
# How long a call may take, in seconds
COMPARE_TIMEOUT = 10


class CompareWorker:
	"""Compares faces with the models through the management daemon, in a thread of its
	own: a call takes a few milliseconds and more, and a frame must not wait for it (the
	camera would deliver the same kind of frame over and over, see the IR emitter). The
	newest face of every position in the frame is kept, the window draws the last answer."""

	def __init__(self):
		self.lock = threading.Lock()
		self.wake = threading.Event()
		self.ready = threading.Event()
		self.stop = False
		self.pending = {}
		# running, no_models or failed
		self.state = "running"
		self.error = ""
		self.matches = {}
		self.start_error = None
		self.thread = threading.Thread(target=self.run, daemon=True)
		self.thread.start()
		self.ready.wait()

	def submit(self, slot, face):
		"""Compare the face at this position of the frame, replaces one that did not start yet"""
		with self.lock:
			self.pending[slot] = face
		self.wake.set()

	def snapshot(self):
		with self.lock:
			return self.state, self.error, dict(self.matches)

	def close(self):
		with self.lock:
			self.stop = True
		self.wake.set()

	def run(self):
		"""The thread must never end silently: the constructor waits for ready, and a thread
		that died would leave the window without matches and without a message"""
		try:
			self.serve()
		except Exception as exc:  # noqa: BLE001
			with self.lock:
				if self.ready.is_set():
					self.state = "failed"
					self.error = str(exc) or type(exc).__name__
				else:
					self.start_error = str(exc) or type(exc).__name__
		finally:
			self.ready.set()

	def serve(self):
		loop = asyncio.new_event_loop()
		# dbus-next looks for the loop of the thread
		asyncio.set_event_loop(loop)
		bus = loop.run_until_complete(MessageBus(bus_type=BusType.SYSTEM).connect())
		self.ready.set()

		next_allowed = time.monotonic()
		while True:
			self.wake.wait()
			# Not more often than every COMPARE_INTERVAL: the label is up to date ten times a
			# second, and the daemon and the bus do not take the time the frames need. Faces that
			# come in meanwhile replace the waiting ones.
			time.sleep(max(next_allowed - time.monotonic(), 0))
			with self.lock:
				if self.stop:
					return
				work, self.pending = self.pending, {}
				self.wake.clear()
			next_allowed = time.monotonic() + COMPARE_INTERVAL

			for slot, face in work.items():
				# An error of the call ends the thread, run() records it
				reply = loop.run_until_complete(
					asyncio.wait_for(
						bus.call(
							Message(
								destination=MANAGEMENT_BUS,
								path=MANAGEMENT_PATH,
								interface=MANAGEMENT_INTERFACE,
								member="Compare",
								signature="ssad",
								body=[user, metric, face],
							)
						),
						timeout=COMPARE_TIMEOUT,
					)
				)
				if reply.message_type == MessageType.ERROR:
					raise RuntimeError(reply.body[0])
				found, value, label = reply.body
				with self.lock:
					if not found:
						self.state = "no_models"
						return
					self.matches[slot] = (value, label)


compare_worker = CompareWorker()
if compare_worker.start_error is not None:
	print(f"Could not reach the management daemon: {compare_worker.start_error}")
	sys.exit(1)

# How long a camera that is there but busy is waited for, and how often it is tried, in seconds
CAMERA_WAIT = 5
CAMERA_RETRY_INTERVAL = 0.25


def open_camera():
	"""Open the camera. A face authentication (pam_howdy) may have used it just before, and
	the camera is busy for a moment after that: the driver needs its time to
	stop the stream and to let go of the device. So a camera that is there but can not be read
	is tried again for a while (reading one frame shows that), instead of a pause that is a
	guess and costs time when the camera is free. A device path that does not exist fails at
	once."""
	device_path = config.get("video", "device_path", fallback="none")
	deadline = time.monotonic() + CAMERA_WAIT
	announced = False

	# The warnings of OpenCV and GStreamer come with every try
	try:
		old_level = cv2.utils.logging.setLogLevel(cv2.utils.logging.LOG_LEVEL_ERROR)
	except AttributeError:
		old_level = None

	try:
		while True:
			try:
				capture = VideoCapture(config)
				# A camera that is opened but still busy fails on the first read
				capture.read_frame()
				return capture
			except NoDeviceException:
				if (
					device_path != "none" and not os.path.exists(device_path)
				) or time.monotonic() >= deadline:
					raise
				if not announced:
					announced = True
					print("Waiting for the camera ...")
				time.sleep(CAMERA_RETRY_INTERVAL)
	finally:
		if old_level is not None:
			cv2.utils.logging.setLogLevel(old_level)


rotate = fixed_rotation(config.getint("video", "rotate", fallback=0))
try:
	video_capture = open_camera()
except NoDeviceException as exc:
	print(exc.message)
	sys.exit(CompareResult.InvalidDevice)


def certainty_to_probability(certainty):
	c = max(0.0, min(1.0, float(certainty)))
	if c >= 0.5:
		return (1.0 - c) * 100.0
	return 100.0 - 50.0 * (2.0 * c) ** (1.0 / 0.569323)


def probability_to_certainty(probability):
	p = max(0.0, min(100.0, float(probability)))
	if p <= 50:
		return 1.0 - p / 100.0
	return 0.5 * ((100.0 - p) / 50.0) ** 0.569323


# Read config values to use in the main loop
video_certainty = round(
	probability_to_certainty(
		config.getfloat("face_authentication", "certainty", fallback=75)
	),
	5,
)
dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)
openvino_device = config.get("video", "openvino_device", fallback="GPU").upper()
identical_threshold = (
	config.getfloat("face_authentication", "identical_threshold", fallback=99.75) / 100
)


# Let the user know what's up
print(
	S("""
Opening a window with a test feed

Press ctrl+C in this terminal to quit
Click on the image to enable or disable slow mode
""")
)


def mouse(event, *_):
	"""Handle mouse events"""
	global slow_mode

	# Toggle slowmode on click
	if event == cv2.EVENT_LBUTTONDOWN:
		slow_mode = not slow_mode


def print_text(line_number, text):
	"""Print the status text by line number"""
	cv2.putText(
		overlay,
		text,
		(px(10), overlay.shape[0] - px(10) - px(10 * line_number)),
		cv2.FONT_HERSHEY_SIMPLEX,
		FONT_SCALE_SMALL,
		(0, 255, 0),
		0,
		cv2.LINE_AA,
	)


use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)

if use_openvino:
	try:
		from .. import openvino_face  # type: ignore

		user = builtins.howdy_args.user
		spoof_detector_flir = openvino_face.SpoofDetectorFlir(
			openvino_device, user=user
		)
		spoof_detector = openvino_face.SpoofDetector(openvino_device, user=user)
		face_detector = openvino_face.FaceDetector(openvino_device, user=user)
		face_encoder = openvino_face.FaceEncoder(openvino_device, user=user)
		micromotion_detector = openvino_face.MicroMotionDetector(
			openvino_device, user=user
		)
		print(f"Using OpenVINO {openvino_device} for face detection and encoding")
		video_certainty = 1 - video_certainty
	except openvino_face.OpenVINOException as exc:
		print(f"OpenVINO init failed: '{exc.message}'", file=sys.stderr)
		print(
			S(
				"OpenVINO data files have not been downloaded, please run the following commands:"
			),
			file=sys.stderr,
		)
		print(f"\tcd {paths_factory.openvino_data_dir_path()}", file=sys.stderr)
		print("\tsudo ./install.sh", file=sys.stderr)
		sys.exit(1)
else:
	if use_cnn:
		face_detector = dlib.cnn_face_detection_model_v1(
			paths_factory.mmod_human_face_detector_path()
		)
	else:
		face_detector = dlib.get_frontal_face_detector()
	pose_predictor = dlib.shape_predictor(
		paths_factory.shape_predictor_5_face_landmarks_path()
	)
	from .. import dlib_face  # type: ignore

	micromotion_detector = dlib_face.MicroMotionDetector()
	face_encoder = dlib.face_recognition_model_v1(
		paths_factory.dlib_face_recognition_resnet_model_v1_path()
	)

texture_detector = anti_spoof_core.TextureDetector()
similarity_detector = anti_spoof_core.SimilarityDetector()

# Off if the user has no model or the daemon can not be reached
compare_enabled = True


def compare(slot, face_encoding):
	"""(value, label) of the best match for the face at this position of the frame, as
	the management daemon answered it for the last frame, None if there is none yet. The
	comparison is switched off if the user has no model, and if the daemon can not be asked"""
	global compare_enabled
	compare_worker.submit(slot, [float(number) for number in face_encoding])
	state, error, matches = compare_worker.snapshot()
	if state == "failed":
		print(
			f"Could not compare the face with the face models ({error}), no matches will be shown\n"
		)
		compare_enabled = False
		return None
	if state == "no_models":
		compare_enabled = False
		return None
	return matches.get(slot)


clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

# Open the window and attach a a mouse listener
cv2.namedWindow("Howdy Test")
cv2.setMouseCallback("Howdy Test", mouse)

# Enable a delay in the loop
slow_mode = False
# Count all frames ever
total_frames = 0
# Count all frames per second
sec_frames = 0
# Last secands FPS
fps = 0
# The current second we're counting
sec = int(time.monotonic())
# recognition time
rec_tm = 0

# Wrap everything in an keyboard interrupt handler
try:
	micromotion_detector.init()
	texture_detector.init()
	similarity_detector.init()
	while True:
		frame_tm = time.monotonic()

		# Increment the frames
		total_frames += 1
		sec_frames += 1

		# Id we've entered a new second
		if sec != int(frame_tm):
			# Set the last seconds FPS
			fps = sec_frames

			# Set the new second and reset the counter
			sec = int(frame_tm)
			sec_frames = 0

		# Grab a single frame of video
		try:
			frame, gsframe = video_capture.read_frame()
		except NoDeviceException as exc:
			print(exc.message)
			sys.exit(CompareResult.InvalidDevice)
		frame = rotate_frame(frame, rotate, 0)
		gsframe = rotate_frame(gsframe, rotate, 0)
		gsframe = clahe.apply(gsframe)

		# The video as it is shown, enlarged, and a copy of it to draw the overlays in
		shown = cv2.resize(
			cv2.flip(cv2.cvtColor(gsframe, cv2.COLOR_GRAY2BGR), 1),
			None,
			fx=DISPLAY_SCALE,
			fy=DISPLAY_SCALE,
			interpolation=cv2.INTER_CUBIC,
		)
		overlay = shown.copy()

		# Fetch the frame height and width
		height, width = gsframe.shape[:2]

		# Create a histogram of the image with 8 values
		# ravel because the returned shape differs across OpenCV versions:
		# 4.x returns (8, 1), 5.x returns (8,)
		hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256]).ravel()
		# All values combined for percentage calculation
		hist_total = int(hist.sum())
		# Fill with the overall containing percentage
		hist_perc = []

		# Loop though all values to calculate a percentage and add it to the overlay
		for index, value in enumerate(hist):
			value_perc = float(value) / hist_total * 100
			hist_perc.append(value_perc)

			# Top left point, 10px margins
			p1 = px(20 + (10 * index), 10)
			# Bottom right point makes the bar 10px thick, with an height of half the percentage
			p2 = px(10 + (10 * index), int(value_perc / 2 + 10))
			# Draw the bar in green
			cv2.rectangle(overlay, p1, p2, (0, 200, 0), thickness=cv2.FILLED)

		# Print the statis in the bottom left
		print_text(0, S("RESOLUTION: %dx%d") % (height, width))
		print_text(1, S("FPS: %d") % (fps,))
		print_text(2, S("FRAMES: %d") % (total_frames,))
		print_text(3, S("RECOGNITION: %dms") % (round(rec_tm * 1000),))

		# Show that slow mode is on, if it's on
		if slow_mode:
			cv2.putText(
				overlay,
				S("SLOW MODE"),
				(overlay.shape[1] - px(66), overlay.shape[0] - px(10)),
				cv2.FONT_HERSHEY_SIMPLEX,
				FONT_SCALE_SMALL,
				(0, 0, 255),
				0,
				cv2.LINE_AA,
			)

		# Ignore dark frames
		if hist_perc[0] > dark_threshold:
			# Show that this is an ignored frame in the top right
			cv2.putText(
				overlay,
				S("DARK FRAME"),
				px(width - 68, 16),
				cv2.FONT_HERSHEY_SIMPLEX,
				FONT_SCALE_SMALL,
				(0, 0, 255),
				0,
				cv2.LINE_AA,
			)
		else:
			# Show that this is an active frame
			cv2.putText(
				overlay,
				S("SCAN FRAME"),
				px(width - 68, 16),
				cv2.FONT_HERSHEY_SIMPLEX,
				FONT_SCALE_SMALL,
				(0, 255, 0),
				0,
				cv2.LINE_AA,
			)

			rec_tm = time.monotonic()

			# Get the locations of all faces and their locations
			# Upsample it once
			face_locations = face_detector(gsframe, 1)
			rec_tm = time.monotonic() - rec_tm

			# The spoof detectors are models of OpenVINO
			if use_openvino:
				print(f"spoof: {round(spoof_detector(frame) * 100, 3)}%")  # Anpassen

			# Loop though all faces and paint a ellipse around them
			for slot, loc in enumerate(face_locations):
				if use_cnn and not use_openvino:
					loc = loc.rect

				if use_openvino:
					flir = spoof_detector_flir(frame, loc)
					print(f"damo-flir: {round(flir * 100, 3)}%")
					movement = micromotion_detector(frame, loc)
				else:
					movement = micromotion_detector(pose_predictor(frame, loc))

				print(f"Micro-movement: {movement:.4f}")

				texture = texture_detector(gsframe, loc)

				if texture is None:
					print("No texture")
				else:
					# Texture-threshold = 35.0?
					print(f"texture: {texture:.3f}")

				# By default the ellipse around the face is red for no match
				color = (0, 0, 230)  # BGR

				half_width = (loc.right() - loc.left()) / 2
				half_height = (loc.bottom() - loc.top()) / 2

				# Get the center X and Y from the rectangular points
				x = width - (int(half_width) + loc.left())
				y = int(half_height) + loc.top()

				# Get the radius from the with of the square
				r_horizontal = int(half_width * 1.2)
				r_vertical = int(half_height * 1.2)

				# If we have models defined for the current user
				if compare_enabled:
					if use_openvino:
						face_encoding = face_encoder.encode(frame, loc)
						if face_encoding is None:
							continue
						similarity, stale = similarity_detector(
							face_encoding, identical_threshold
						)
						if similarity is not None:
							print(f"similarity: {similarity * 100:.3f}")
						print(f"stale: {stale}")
						found = compare(slot, face_encoding)
						if found is None:
							continue
						match, label = found
						certainty = certainty_to_probability(1.0 - match)
						accept = video_certainty < match < 1
					else:
						face_landmark = pose_predictor(frame, loc)
						face_encoding = np.array(
							face_encoder.compute_face_descriptor(
								frame, face_landmark, 1
							)
						)
						found = compare(slot, face_encoding)
						if found is None:
							continue
						match, label = found
						certainty = certainty_to_probability(match)
						accept = video_certainty > match > 0

					# If a model matches
					if accept:
						# Turn the ellipse green
						color = (0, 230, 0)  # BGR

						# Print the name of the model next to the ellipse
						ellipse_text = f"{label} ({certainty:.3f})"
						cv2.putText(
							overlay,
							ellipse_text,
							px(int(x + r_horizontal / 3), y - r_vertical),
							cv2.FONT_HERSHEY_SIMPLEX,
							FONT_SCALE_NORMAL,
							color,
							1,
							cv2.LINE_AA,
						)
					# If no approved matches, show red text
					else:
						cv2.putText(
							overlay,
							f"no match ({certainty:.3f})",
							px(int(x + r_horizontal / 3), y - r_vertical),
							cv2.FONT_HERSHEY_SIMPLEX,
							FONT_SCALE_NORMAL,
							color,
							1,
							cv2.LINE_AA,
						)

				# Draw the Ellipse
				cv2.ellipse(
					overlay,
					px(x, y),
					px(r_horizontal, r_vertical),
					0,
					0,
					360,
					color,
					px(3),
				)  # Thickness 2 or 3?

			print("-------------------------------------------------------")
		# Add the overlay to the frame with some transparency
		alpha = 0.65
		cv2.addWeighted(overlay, alpha, shown, 1 - alpha, 0, shown)

		cv2.imshow("Howdy Test", shown)

		# Quit on any keypress
		# if cv2.waitKey(1) != -1:
		if cv2.waitKey(1) & 0xFF == ord("q"):
			raise KeyboardInterrupt()

		# Quit if the window has been closed, imshow would otherwise open a new one
		# The Qt backend throws an error instead of returning -1 for a closed window
		try:
			window_closed = (
				cv2.getWindowProperty("Howdy Test", cv2.WND_PROP_VISIBLE) < 1
			)
		except cv2.error:
			window_closed = True

		if window_closed:
			raise KeyboardInterrupt()

		frame_time = time.monotonic() - frame_tm

		# Delay the frame if slowmode is on
		if slow_mode:
			time.sleep(max([0.5 - frame_time, 0.0]))

# On ctrl+C
except KeyboardInterrupt:
	# Let the user know we're stopping
	print(S("\nClosing window"))

	# Release handle to the webcam
	cv2.destroyAllWindows()

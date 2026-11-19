# Show a window with the video stream and testing information

# Import required modules
import cv2
import sys
import dlib
import json
import time
import builtins
import configparser

import numpy as np

from ..i18n import S  # type: ignore
from .. import paths_factory  # type: ignore
from .. import anti_spoof_core  # type: ignore
from ..exit_codes import CompareResult  # type: ignore
from ..enroll_capture import flatten_models  # type: ignore
from ..rotation import fixed_rotation, rotate_frame  # type: ignore
from ..recorders.video_capture import VideoCapture, NoDeviceException

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

rotate = fixed_rotation(config.getint("video", "rotate", fallback=0))
try:
	video_capture = VideoCapture(config)
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
		(10, height - 10 - (10 * line_number)),
		cv2.FONT_HERSHEY_SIMPLEX,
		0.3,
		(0, 255, 0),
		0,
		cv2.LINE_AA,
	)


use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)
use_openvino = config.getboolean("face_authentication", "use_openvino", fallback=False)

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
	face_encoder = dlib.face_recognition_model_v1(
		paths_factory.dlib_face_recognition_resnet_model_v1_path()
	)

texture_detector = anti_spoof_core.TextureDetector()
similarity_detector = anti_spoof_core.SimilarityDetector()

encodings = []
models = None

try:
	user = builtins.howdy_args.user
	models = json.load(open(paths_factory.user_model_path(user)))
	encoding_size = 256 if use_openvino else 128
	encodings, model_owner = flatten_models(models, encoding_size)
except FileNotFoundError:
	pass

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

		# Make a frame to put overlays in
		overlay = cv2.flip(cv2.cvtColor(gsframe.copy(), cv2.COLOR_GRAY2BGR), 1)

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
			p1 = (20 + (10 * index), 10)
			# Bottom right point makes the bar 10px thick, with an height of half the percentage
			p2 = (10 + (10 * index), int(value_perc / 2 + 10))
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
				(width - 66, height - 10),
				cv2.FONT_HERSHEY_SIMPLEX,
				0.3,
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
				(width - 68, 16),
				cv2.FONT_HERSHEY_SIMPLEX,
				0.3,
				(0, 0, 255),
				0,
				cv2.LINE_AA,
			)
		else:
			# Show that this is an active frame
			cv2.putText(
				overlay,
				S("SCAN FRAME"),
				(width - 68, 16),
				cv2.FONT_HERSHEY_SIMPLEX,
				0.3,
				(0, 255, 0),
				0,
				cv2.LINE_AA,
			)

			rec_tm = time.monotonic()

			# Get the locations of all faces and their locations
			# Upsample it once
			face_locations = face_detector(gsframe, 1)
			rec_tm = time.monotonic() - rec_tm

			print(f"spoof: {round(spoof_detector(frame) * 100, 3)}%")  # Anpassen

			# Loop though all faces and paint a ellipse around them
			for loc in face_locations:
				if use_cnn and not use_openvino:
					loc = loc.rect

				print(f"damo-flir: {round(spoof_detector_flir(frame, loc) * 100, 3)}%")

				print(f"Micro-movement: {micromotion_detector(frame, loc):.4f}")

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
				if models:
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
						matches = np.dot(np.array(encodings), face_encoding)
						match_index = np.argmax(matches)
						match = matches[match_index]
						certainty = certainty_to_probability(1.0 - match)
						accept = video_certainty < match < 1
					else:
						face_landmark = pose_predictor(frame, loc)
						face_encoding = np.array(
							face_encoder.compute_face_descriptor(
								frame, face_landmark, 1
							)
						)
						matches = np.linalg.norm(encodings - face_encoding, axis=1)
						match_index = np.argmin(matches)
						match = matches[match_index]
						certainty = certainty_to_probability(match)
						accept = video_certainty > match > 0

					# If a model matches
					if accept:
						# Turn the ellipse green
						color = (0, 230, 0)  # BGR

						# Print the name of the model next to the ellipse
						ellipse_text = f"{models[model_owner[match_index]]['label']} ({certainty:.3f})"
						cv2.putText(
							overlay,
							ellipse_text,
							(int(x + r_horizontal / 3), y - r_vertical),
							cv2.FONT_HERSHEY_SIMPLEX,
							0.5,
							color,
							1,
							cv2.LINE_AA,
						)
					# If no approved matches, show red text
					else:
						cv2.putText(
							overlay,
							f"no match ({certainty:.3f})",
							(int(x + r_horizontal / 3), y - r_vertical),
							cv2.FONT_HERSHEY_SIMPLEX,
							0.5,
							color,
							1,
							cv2.LINE_AA,
						)

				# Draw the Ellipse
				cv2.ellipse(
					overlay, (x, y), (r_horizontal, r_vertical), 0, 0, 360, color, 3
				)  # Thickness 2 or 3?

			print("-------------------------------------------------------")
		# Add the overlay to the frame with some transparency
		alpha = 0.65
		gsframe = cv2.flip(cv2.cvtColor(gsframe, cv2.COLOR_GRAY2BGR), 1)
		cv2.addWeighted(overlay, alpha, gsframe, 1 - alpha, 0, gsframe)

		# Show the image in a window
		cv2.imshow("Howdy Test", gsframe)

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

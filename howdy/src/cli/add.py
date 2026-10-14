# Save the face of the user in encoded form

# Import required modules
import time
import os
import sys
import json
import configparser
import builtins
import numpy as np
import paths_factory
import enroll_capture

from recorders.video_capture import VideoCapture
from i18n import _

def run(dialog=None):
	# Try to import dlib and give a nice error if we can't
	# Add should be the first point where import issues show up
	try:
		import dlib
	except ImportError as err:
		reason = str(err)
		reason += _("\nCan't import the dlib module, check the output of")
		reason += "pip3 show dlib"
		if builtins.howdy_args.gui:
			return 1, reason
		print(reason)
		sys.exit(1)

	# Test if at lest 1 of the data files is there and abort if it's not
	if not os.path.isfile(paths_factory.shape_predictor_5_face_landmarks_path()):
		reason = _("Data files have not been downloaded, please run the following commands:")
		reason += "\n\tcd " + paths_factory.dlib_data_dir_path()
		reason += "\tsudo ./install.sh\n"
		if builtins.howdy_args.gui:
			return 1, reason
		print(reason)
		sys.exit(1)

	# Read config from disk
	config = configparser.ConfigParser()
	config.read(paths_factory.config_file_path())

	use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)
	use_openvino = config.getboolean("face_authentication", "use_openvino", fallback=False)

	pose_predictor = None

	if use_openvino:
		try:
			import openvino_face
			if openvino_face.is_available():
				face_detector = openvino_face.FaceDetector("GPU")
				face_encoder = openvino_face.FaceEncoder("GPU")
				print("Using OpenVINO GPU for face detection and encoding")
		except Exception as e:
			use_openvino = False
			print(f"OpenVINO not available ({e}), using dlib")

	if not use_openvino:
		if use_cnn:
			face_detector = dlib.cnn_face_detection_model_v1(paths_factory.mmod_human_face_detector_path())
		else:
			face_detector = dlib.get_frontal_face_detector()
		pose_predictor = dlib.shape_predictor(paths_factory.shape_predictor_5_face_landmarks_path())
		face_encoder = dlib.face_recognition_model_v1(paths_factory.dlib_face_recognition_resnet_model_v1_path())


	user = builtins.howdy_args.user
	# The permanent file to store the encoded model in
	enc_file = paths_factory.user_model_path(user)
	# Known encodings
	encodings = []

	# Make the ./models folder if it doesn't already exist
	if not os.path.exists(paths_factory.user_models_dir_path()):
		print(_("No face model folder found, creating one"))
		os.makedirs(paths_factory.user_models_dir_path(), mode=0o755)
	# To try read a premade encodings file if it exists
	try:
		encodings = json.load(open(enc_file))
	except FileNotFoundError:
		encodings = []

	# Print a warning if too many encodings are being added
	if len(encodings) > 3:
		print(_("NOTICE: Each additional model slows down the face recognition engine slightly"))
		print(_("Press Ctrl+C to cancel\n"))

	# Make clear what we are doing if not human
	if not builtins.howdy_args.plain:
		print(_("Adding face model for the user ") + user)

	# Set the default label
	label = "Initial model"

	# some id's can be skipped, but the last id is always the maximum
	next_id = encodings[-1]["id"] + 1 if encodings else 0

	# Get the label from the cli arguments if provided
	if builtins.howdy_args.arguments:
		label = builtins.howdy_args.arguments[0]

	# Or set the default label
	else:
		label = _("Model #") + str(next_id)

	# Keep the default name if we can't ask questions (-y, or no TTY / closed stdin
	# as when the setup wizard / Settings runs add with capture_output).
	if builtins.howdy_args.y or not sys.stdin.isatty():
		print(_('Using default label "%s" because of -y flag') % (label, ))
	else:
		# Ask the user for a custom label
		try:
			label_in = input(_("Enter a label for this new model [{}]: ").format(label))
		except EOFError:
			label_in = ""

		# Set the custom label (if any) and limit it to 24 characters
		if label_in != "":
			label = label_in[:24]

	# Remove illegal characters
	if "," in label:
		print(_("NOTICE: Removing illegal character \",\" from model name"))
		label = label.replace(",", "")

	# Prepare the metadata for insertion
	insert_model = {
		"time": int(time.time()),
		"label": label,
		"id": next_id,
		"data": []
	}

	# Set up video_capture
	video_capture = VideoCapture(config)

	# The wizard / Settings read our stdout through a pipe: flush every line so
	# the on-screen guidance follows the capture in real time.
	def say(line):
		print(line, flush=True)

	dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)

	# First sample: the frontal frame that started the scan. Then guide the user
	# through small head movements and keep a few more descriptors per pose so
	# the model covers more than one snapshot (see enroll_capture.py).
	try:
		samples = enroll_capture.capture_guided_samples(
			say=say,
			use_cnn=use_cnn,
			use_openvino=use_openvino,
			face_encoder=face_encoder,
			face_detector=face_detector,
			video_capture=video_capture,
			pose_predictor=pose_predictor,
			dark_threshold=dark_threshold,
			dialog=dialog,
		)
	except Exception as e:
		if builtins.howdy_args.gui:
			raise e
		print(e)
		return
	finally:
		# Release the camera on success and on any detection / encoding / read error
		# (the wizard's live preview reopens the device right after add exits).
		video_capture.release()

	if not samples:
		if builtins.howdy_args.gui:
			raise Exception(_("No samples captured, aborting"))
		return

	insert_model["data"].extend(samples)
	say(_(f"Captured {len(samples)} face samples"))

	# Insert full object into the list
	encodings.append(insert_model)

	# Save the new encodings to disk
	with open(enc_file, "w") as datafile:
		json.dump(encodings, datafile)
	os.chmod(enc_file, 0o644)

	# Give let the user know how it went
	say(_(f"\nScan complete\nAdded a new model to {user}"))

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

# Try to import dlib and give a nice error if we can't
# Add should be the first point where import issues show up
try:
	import dlib
except ImportError as err:
	print(err)

	print(_("\nCan't import the dlib module, check the output of"))
	print("pip3 show dlib")
	sys.exit(1)

# OpenCV needs to be imported after dlib
import cv2

# Test if at lest 1 of the data files is there and abort if it's not
if not os.path.isfile(paths_factory.shape_predictor_5_face_landmarks_path()):
	print(_("Data files have not been downloaded, please run the following commands:"))
	print("\n\tcd " + paths_factory.dlib_data_dir_path())
	print("\tsudo ./install.sh\n")
	sys.exit(1)

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

use_cnn = config.getboolean("core", "use_cnn", fallback=False)
if use_cnn:
	face_detector = dlib.cnn_face_detection_model_v1(paths_factory.mmod_human_face_detector_path())
else:
	face_detector = dlib.get_frontal_face_detector()

pose_predictor = dlib.shape_predictor(paths_factory.shape_predictor_5_face_landmarks_path())
face_encoder = dlib.face_recognition_model_v1(paths_factory.dlib_face_recognition_resnet_model_v1_path())

user = builtins.howdy_user
# The permanent file to store the encoded model in
enc_file = paths_factory.user_model_path(user)
# Known encodings
encodings = []

# Make the ./models folder if it doesn't already exist
if not os.path.exists(paths_factory.user_models_dir_path()):
	print(_("No face model folder found, creating one"))
	os.makedirs(paths_factory.user_models_dir_path())

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


# The same prompts go to a desktop notification card: the banner sits right
# under the camera, where the user is looking during the scan.
progress = {"count": 0, "total": 0, "prompt": ""}


def emit(line):
	"""Print one capture line; mirror prompts / progress onto the notification."""
	if line.startswith("@progress "):
		try:
			count, total = line[len("@progress "):].split("/", 1)
			progress["count"], progress["total"] = int(count), int(total)
		except ValueError:
			pass
		if builtins.howdy_args.gui:
			say(line)
		return
	if line.startswith("@"):
		if builtins.howdy_args.gui:
			say(line)
		return
	text = _(line)
	say(text)


emit("@guide center")
emit(_("\nPlease look straight into the camera"))

# Give the user time to read
time.sleep(2)

# Count the number of read frames
frames = 0
# Count the number of illuminated read frames
valid_frames = 0
# Count the number of illuminated frames that
# were rejected for being too dark
dark_tries = 0
# Track the running darkness total
dark_running_total = 0
face_locations = None

dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)

clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))


def usable_frame():
	"""Read one frame; returns (frame, gsframe) or (frame, None) when it is black / too dark."""
	global valid_frames, dark_tries, dark_running_total
	frame, gsframe = video_capture.read_frame()
	gsframe = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
	gsframe = clahe.apply(gsframe)

	# If the image is fully black due to a bad camera read, skip it
	darkness = enroll_capture.frame_darkness(gsframe, np, cv2)
	if darkness is None or darkness == 100:
		return frame, None

	# Include this frame in calculating our average session brightness
	dark_running_total += darkness
	valid_frames += 1

	# If the image exceeds darkness threshold due to subject distance, skip it
	if darkness > dark_threshold:
		dark_tries += 1
		return frame, None
	return frame, gsframe


def face_rect(face_location):
	return face_location.rect if use_cnn else face_location


def encode(frame, face_location):
	face_landmark = pose_predictor(frame, face_rect(face_location))
	return np.array(face_encoder.compute_face_descriptor(frame, face_landmark, 1)).tolist()


# Loop through frames till we hit a timeout
while frames < 60:
	frames += 1
	frame, gsframe = usable_frame()
	if gsframe is None:
		continue

	# Get all faces from that frame as encodings
	face_locations = face_detector(gsframe, 1)

	# If we've found at least one, we can continue
	if face_locations:
		break

# If we've found no faces, try to determine why
if not face_locations:
	video_capture.release()
	if valid_frames == 0:
		reason = _("Camera saw only black frames - is IR emitter working?")
	elif valid_frames == dark_tries:
		reason = _("All frames were too dark, please check dark_threshold in config") + " " + \
			_("Average darkness: {avg}, Threshold: {threshold}").format(avg=str(dark_running_total / valid_frames), threshold=str(dark_threshold))
	else:
		reason = _("No face detected, aborting")
	print(reason)
	sys.exit(1)

# If more than 1 faces are detected we can't know which one belongs to the user
elif len(face_locations) > 1:
	video_capture.release()
	print(_("Multiple faces detected, aborting"))
	sys.exit(1)

# First sample: the frontal frame that started the scan. Then guide the user
# through small head movements and keep a few more descriptors per pose so
# the model covers more than one snapshot (see enroll_capture.py).
try:
	first_sample = encode(frame, face_locations[0])
	samples = enroll_capture.capture_guided_samples(
		read_frame=usable_frame,
		detect_faces=lambda gs: face_detector(gs, 1),
		encode_face=encode,
		emit=emit,
		first_sample=first_sample,
	)
finally:
	# Release the camera on success and on any detection / encoding / read error
	# (the wizard's live preview reopens the device right after add exits).
	video_capture.release()

insert_model["data"].extend(samples)
say(_("Captured {} face samples").format(len(samples)))

# Insert full object into the list
encodings.append(insert_model)

# Save the new encodings to disk
with open(enc_file, "w") as datafile:
	json.dump(encodings, datafile)
os.chmod(enc_file, 0o644)

# Give let the user know how it went
say(_("""\nScan complete
Added a new model to """) + user)

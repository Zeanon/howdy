# Save the face of the user in encoded form

# Import required modules
import os
import sys
import json
import time
import asyncio
import builtins
import configparser

from ..i18n import S
from ..model_limits import many_models, many_models_warning
from .. import enroll_helper
from .. import paths_factory
from .. import enroll_capture
from .list import list_models
from .auth import check_authorization
from ..recorders.video_capture import VideoCapture, NoDeviceException

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType


MANAGEMENT_BUS = "@management_bus@"
MANAGEMENT_PATH = "@management_path@"
MANAGEMENT_INTERFACE = "@management_interface@"


def exit(reason):
	if builtins.howdy_args.gui:
		if isinstance(reason, Exception):
			raise reason
		raise Exception(reason)
	if isinstance(reason, enroll_helper.EnrollCancelled):
		sys.exit(enroll_helper.EXIT_CANCELLED)
	print(reason)
	sys.exit(1)


def say(line):
	if not builtins.howdy_args.gui:
		print(line)


async def send_model(bus, model):
	signature = "ss" if builtins.howdy_args.user else "s"
	body = [builtins.howdy_args.user] if builtins.howdy_args.user else []
	body.append(json.dumps(model))

	reply = await bus.call(
		Message(
			destination=MANAGEMENT_BUS,
			path=MANAGEMENT_PATH,
			interface=MANAGEMENT_INTERFACE,
			member="Add",
			signature=signature,
			body=body,
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(reply.body[0])

	if not reply.body[0]:
		raise RuntimeError("Failed to add face model")


def run(dialog=None):
	asyncio.run(_run(dialog=dialog))


async def _run(dialog=None):
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	try:
		user = builtins.howdy_args.user
		try:
			await check_authorization(
				bus=bus, caller_uid=os.geteuid(), target_user=user
			)
		except Exception as exc:
			exit(f"Authorization failed: {exc}")

		# Read config from disk
		config = configparser.ConfigParser()
		config.read(paths_factory.config_file_path())

		use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)
		use_openvino = config.getboolean(
			"face_authentication", "use_openvino", fallback=False
		)
		openvino_device = config.get("video", "openvino_device", fallback="GPU").upper()

		pose_predictor = None

		if use_openvino:
			try:
				from .. import openvino_face

				try:
					face_detector = openvino_face.FaceDetector(
						device=openvino_device,
						user=user,
					)
					face_encoder = openvino_face.FaceEncoder(
						device=openvino_device, user=user
					)
					if not builtins.howdy_args.plain:
						say(
							f"Using OpenVINO {openvino_device} for face detection and encoding"
						)
				except openvino_face.OpenVINOException as exc:
					reason = f"OpenVINO init failed: '{exc.message}'"
					reason += S(
						"OpenVINO data files have not been downloaded, please run the following commands:"
					)
					reason += "\tcd " + paths_factory.openvino_data_dir_path()
					reason += "\tsudo ./install.sh"
					exit(reason)
			except Exception as exc:
				exit(f"OpenVINO not available ({exc})")
		else:
			# Try to import dlib and give a nice error if we can't
			# Add should be the first point where import issues show up
			try:
				import dlib
			except ImportError as err:
				reason = str(err)
				reason += S("\nCan't import the dlib module, check the output of")
				reason += "\npip3 show dlib"
				exit(reason)

			def error_message(reason):
				reason += S(
					"DLIB data files have not been downloaded, please run the following commands:"
				)
				reason += "\tcd " + paths_factory.dlib_data_dir_path()
				reason += "\tsudo ./install.sh"
				exit(reason)

			if use_cnn:
				if not os.path.isfile(paths_factory.mmod_human_face_detector_path()):
					error_message(S("DLib Face-Detector model does not exist\n"))
				face_detector = dlib.cnn_face_detection_model_v1(
					paths_factory.mmod_human_face_detector_path()
				)
			else:
				face_detector = dlib.get_frontal_face_detector()

			if not os.path.isfile(
				paths_factory.shape_predictor_5_face_landmarks_path()
			):
				error_message(S("DLib Landmark-Model model does not exist\n"))
			pose_predictor = dlib.shape_predictor(
				paths_factory.shape_predictor_5_face_landmarks_path()
			)

			if not os.path.isfile(
				paths_factory.dlib_face_recognition_resnet_model_v1_path()
			):
				error_message(S("DLib Face-Encoder model does not exist\n"))
			face_encoder = dlib.face_recognition_model_v1(
				paths_factory.dlib_face_recognition_resnet_model_v1_path()
			)

		try:
			encodings = await list_models(bus)
		except Exception:
			encodings = []

		# Print a warning if too many encodings are being added
		if many_models(len(encodings)):
			say(S("NOTICE: ") + many_models_warning())
			say(S("Press Ctrl+C to cancel\n"))

		# Make clear what we are doing if not human
		if not builtins.howdy_args.plain:
			say(S("Adding face model for the user ") + user)

		label = S("Model #") + str(encodings[-1]["id"] + 1 if encodings else 0)
		# Get the label from the cli arguments if provided, an empty one (an empty field
		# of the settings window) keeps the default
		if builtins.howdy_args.arguments and builtins.howdy_args.arguments[0].strip():
			label = builtins.howdy_args.arguments[0][:24]

		# Keep the default name if we can't ask questions (-y, or no TTY / closed stdin
		# as when the setup wizard / Settings runs add with capture_output).
		if builtins.howdy_args.y or not sys.stdin.isatty():
			if not builtins.howdy_args.gui:
				say(S('Using default label "{}" because of -y flag').format(label))
		else:
			# Ask the user for a custom label
			try:
				label_in = input(
					S("Enter a label for this new model [{}]: ").format(label)
				)
			except EOFError:
				label_in = ""

			# Set the custom label (if any) and limit it to 24 characters
			if label_in != "":
				label = label_in[:24]

		# Remove illegal characters
		if "," in label:
			say(S('NOTICE: Removing illegal character "," from model name'))
			label = label.replace(",", "")

		# Prepare the metadata for insertion
		insert_model = {"time": int(time.time()), "label": label, "data": []}

		# Set up video_capture
		try:
			video_capture = VideoCapture(config)
		except NoDeviceException as exc:
			exit(exc.message)

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
				rotate=config.getint("video", "rotate", fallback=0),
			)
		except Exception as exc:
			exit(exc)
		finally:
			# Release the camera on success and on any detection / encoding / read error
			# (the wizard's live preview reopens the device right after add exits).
			# A camera that is still being read is released when that has ended
			if not getattr(video_capture, "still_reading", False):
				video_capture.release()

		if not samples:
			reason = S("No samples captured, aborting")
			exit(reason)

		insert_model["data"].extend(samples)
		say(S("Captured {} face samples").format(len(samples)))

		try:
			await send_model(bus, insert_model)
		except Exception as exc:
			exit(f"Failed to save face model: {exc}")

		# Give let the user know how it went
		say(S("\nScan complete\nAdded a new model to {}").format(user))
	finally:
		bus.disconnect()

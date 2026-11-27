import os
import cv2
import pwd
import sys
import json
import time
import uuid
import syslog
import threading
import traceback
import configparser
import contextvars

from datetime import datetime, UTC

import numpy as np

from enum import StrEnum
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor

from dbus_next import DBusError, MessageType
from dbus_next.service import ServiceInterface, method

from .i18n import S
from . import paths_factory
from . import snapshot
from .caller import CallerError, is_privileged, may_access, query_caller_uid
from .file_util import read_model_file
from .enroll_capture import flatten_models
from .rotation import rotate_frame
from .recorders.video_capture import VideoCapture, NoDeviceException


AUTHENTICATION_INTERFACE = "@authentication_interface@"

# The longest timeout a caller without privileges can ask for, if the config does not
# allow more
UNPRIVILEGED_MAX_TIMEOUT = 60


@dataclass
class Args:
	auth_popup: bool
	timeout: float


class HowdyStatus(StrEnum):
	Running = "RUNNING"
	Success = "SUCCESS"
	NoMatch = "NO_MATCH"
	NoEnrolledFaces = "NO_ENROLLED_FACE"
	Timeout = "TIMEOUT"
	TooDark = "TOO_DARK"
	Cancelled = "CANCELLED"
	Cancelling = "CANCELLING"
	Error = "ERROR"
	NotInitialized = "NOT_INITIALIZED"
	NoDevice = "NO_DEVICE"
	KeepAliveTimeout = "KEEPALIVE_TIMEOUT"


def str_to_seconds(timestring):
	if not isinstance(timestring, str):
		return timestring

	i = 1 if timestring[0] == "-" else 0
	while i < len(timestring) and timestring[i].isdigit():
		i += 1

	timeout = int(timestring[:i])
	if i == len(timestring):
		return timeout

	unit = timestring[i]
	if unit == "d":
		return timeout * 86400
	if unit == "h":
		return timeout * 3600
	if unit == "m":
		return timeout * 60
	return timeout


def str_to_bool(boolstring):
	return boolstring.lower() in ["true", "yes", "on", "y", "1"]


def probability_to_certainty(probability):
	p = max(0.0, min(100.0, float(probability)))
	if p <= 50:
		return 1.0 - p / 100.0
	return 0.5 * ((100.0 - p) / 50.0) ** 0.569323


class HowdyAuthenticator:
	def __init__(self, auth_ui):
		self._camera_lock = threading.Lock()
		self._initialized = False
		self._cleaned_up = False

		self.auth_ui = auth_ui
		self.ui_uid = None
		self.video_capture = None

		self.face_encoder = None
		self.face_detector = None
		self.pose_predictor = None
		self.spoof_detector = None
		self.spoof_detector_flir = None
		self.micromotion_detector = None
		self.texture_detector = None
		self.similarity_detector = None

		self._read_config()
		# Old snapshots go away even if no new ones are saved any more
		self._prune_snapshots()
		self._initialize_models()

		self._initialized = True

	def has_face_model(self, username):
		"""Whether the user has a face model, otherwise there is nothing to look for.
		True if that can not be said, the authentication then reports what is wrong."""
		try:
			models = json.loads(
				read_model_file(paths_factory.user_model_path(username), username)
			)
			encodings, _ = flatten_models(models, 256 if self.use_openvino else 128)
			return len(encodings) > 0
		except FileNotFoundError:
			return False
		except Exception:
			return True

	def _read_config(self):
		self.config = configparser.ConfigParser()
		self.config.read(paths_factory.config_file_path())

		# Get all config values needed
		self.use_cnn = self.config.getboolean(
			"face_authentication", "use_cnn", fallback=False
		)
		self.use_openvino = self.config.getboolean(
			"face_authentication", "use_openvino", fallback=False
		)
		self.timeout = str_to_seconds(
			self.config.get("face_authentication", "timeout", fallback="4s")
		)
		# How long a caller without privileges has to wait after its authentication
		# before it can start the next one, so it can not keep the camera busy
		self.start_cooldown = max(
			0,
			str_to_seconds(
				self.config.get("face_authentication", "start_cooldown", fallback="2s")
			),
		)
		certainty = self.config.getfloat(
			"face_authentication", "certainty", fallback=75
		)
		# The setup of an older version has written values of another scale (2.2 to 4.2)
		if certainty < 50:
			syslog.syslog(
				syslog.LOG_WARNING,
				f"The certainty of {certainty} in [face_authentication] is very low, faces "
				"of other people may be accepted. It is a percentage, the default is 75",
			)
		self.certainty = round(probability_to_certainty(certainty), 5)
		# Invert certainty if we use openvino as for openvino 1 equals full match, while for dlib 0 equals full match
		if self.use_openvino:
			self.certainty = 1 - self.certainty
		self.confirmations = max(
			1, self.config.getint("face_authentication", "confirmations", fallback=5)
		)

		self.save_failed = self.config.getboolean(
			"snapshots", "save_failed", fallback=False
		)
		self.save_successful = self.config.getboolean(
			"snapshots", "save_successful", fallback=False
		)
		# How long and how many snapshots are kept, zero means no limit
		self.snapshot_max_age = max(
			0, str_to_seconds(self.config.get("snapshots", "retention", fallback="7d"))
		)
		self.snapshot_max_count = max(
			0, self.config.getint("snapshots", "max_snapshots", fallback=200)
		)

		self.use_anti_spoof = self.config.getboolean(
			"face_authentication", "use_anti_spoof", fallback=True
		)
		self.anti_spoof_threshold = 1 - (
			self.config.getfloat(
				"face_authentication", "anti_spoof_threshold", fallback=95
			)
			/ 100
		)
		self.use_damo_flir = self.config.getboolean(
			"face_authentication", "use_damo_flir", fallback=True
		)
		self.damo_flir_threshold = (
			self.config.getfloat(
				"face_authentication", "damo_flir_threshold", fallback=95
			)
			/ 100
		)

		self.use_micromotion_detection = self.config.getboolean(
			"face_authentication", "micromotion_detection", fallback=True
		)
		self.micromotion_detection_buffer_size = self.config.getint(
			"face_authentication", "micromotion_detection_buffer_size", fallback=8
		)
		self.micromotion_threshold = self.config.getfloat(
			"face_authentication", "micromotion_threshold", fallback=0.03
		)

		self.use_texture_detection = self.config.getboolean(
			"face_authentication", "texture_detection", fallback=True
		)
		self.texture_detection_buffer_size = self.config.getint(
			"face_authentication", "texture_detection_buffer_size", fallback=5
		)
		self.texture_threshold = self.config.getfloat(
			"face_authentication", "texture_threshold", fallback=35.0
		)

		self.use_similarity_detection = self.config.getboolean(
			"face_authentication", "similarity_detection", fallback=True
		)
		self.similarity_detection_buffer_size = self.config.getint(
			"face_authentication", "similarity_detection_buffer_size", fallback=8
		)
		self.identical_threshold = (
			self.config.getfloat(
				"face_authentication", "identical_threshold", fallback=99.75
			)
			/ 100
		)
		self.sudden_change_threshold = (
			self.config.getfloat(
				"face_authentication", "sudden_change_threshold", fallback=90.0
			)
			/ 100
		)

		self.auth_popup = self.config.getboolean(
			"face_authentication", "show_auth_popup", fallback=False
		)
		self.dark_threshold = self.config.getfloat(
			"video", "dark_threshold", fallback=50.0
		)
		self.gtk_stdout = self.config.getboolean("debug", "gtk_stdout", fallback=False)
		self.rotate = self.config.getint("video", "rotate", fallback=0)
		self.openvino_device = (
			self.config.get("video", "openvino_device", fallback="GPU")
			.upper()
			.strip('"')
		)

	def _parse_args(self, args, limit_timeout=False):
		"""The only settings of a request are show-auth-popup and timeout,
		everything else comes from the config file"""
		result = Args(auth_popup=self.auth_popup, timeout=self.timeout)
		for arg in args:
			if arg == "show-auth-popup":
				result.auth_popup = True
			elif arg.startswith("timeout="):
				result.timeout = str_to_seconds(arg[8:])
			elif arg.startswith("show-auth-popup="):
				result.auth_popup = str_to_bool(arg[16:])
			else:
				syslog.syslog(
					syslog.LOG_WARNING, f"Ignoring the unknown argument '{arg}'"
				)
		# A caller without privileges can not keep the camera for longer than the config
		# says (or a minute): others get Busy meanwhile
		if limit_timeout and self.timeout >= 1:
			longest = max(self.timeout, UNPRIVILEGED_MAX_TIMEOUT)
			if result.timeout < 1 or result.timeout > longest:
				syslog.syslog(
					syslog.LOG_WARNING,
					f"The requested timeout is too long for this caller, using {longest}s",
				)
				result.timeout = longest

		# If timeout is below 1, we set it to sys.maxsize which is basically infinite
		if result.timeout < 1:
			result.timeout = sys.maxsize
		return result

	def _initialize_models(self):
		# fmt: off
		# Start face detector, encoder and predictor
		if self.use_openvino:
			try:
				from . import openvino_face # type: ignore

				try:
					self.face_encoder = openvino_face.FaceEncoder(device=self.openvino_device)
					self.face_detector = openvino_face.FaceDetector(
						device=self.openvino_device
					)
					if self.use_anti_spoof:
						self.spoof_detector = openvino_face.SpoofDetector(device=self.openvino_device)
					if self.use_damo_flir:
						self.spoof_detector_flir = openvino_face.SpoofDetectorFlir(device=self.openvino_device)
					if self.use_micromotion_detection:
						self.micromotion_detector = openvino_face.MicroMotionDetector(device=self.openvino_device)
					# The nod rubber stamp needs the landmarks, also without micromotion detection
					if self.config.getboolean("rubberstamps", "enabled", fallback=False):
						landmarks = self.micromotion_detector or openvino_face.MicroMotionDetector(device=self.openvino_device)
						self.pose_predictor = landmarks.landmarks
				except openvino_face.OpenVINOException as exc:
					syslog.syslog(syslog.LOG_ERR, f"OpenVINO init failed: '{exc.message}'")
					syslog.syslog(syslog.LOG_ERR, S("OpenVINO data files have not been downloaded, please run the following commands:"))
					syslog.syslog(syslog.LOG_ERR, f"\tcd {paths_factory.openvino_data_dir_path()}")
					syslog.syslog(syslog.LOG_ERR, "\tsudo ./install.sh")
					sys.exit(1)
			except Exception:
				syslog.syslog(syslog.LOG_ERR, "OpenVINO init failed")
				syslog.syslog(syslog.LOG_ERR, traceback.format_exc())
				sys.exit(1)
		else:
			import dlib

			def error_message():
				syslog.syslog(syslog.LOG_ERR, S("DLIB data files have not been downloaded, please run the following commands:"))
				syslog.syslog(syslog.LOG_ERR, f"\tcd {paths_factory.dlib_data_dir_path()}")
				syslog.syslog(syslog.LOG_ERR, "\tsudo ./install.sh")
				sys.exit(1)

			if self.use_cnn:
				if not os.path.isfile(
					paths_factory.mmod_human_face_detector_path()
				):
					syslog.syslog(syslog.LOG_ERR, "DLib Face-Detector model does not exist")
					error_message()
				self.face_detector = dlib.cnn_face_detection_model_v1(
					paths_factory.mmod_human_face_detector_path()
				)
			else:
				self.face_detector = dlib.get_frontal_face_detector()

			if not os.path.isfile(
				paths_factory.shape_predictor_5_face_landmarks_path()
			):
				syslog.syslog(syslog.LOG_ERR, "DLib Landmark-Model model does not exist")
				error_message()
			self.pose_predictor = dlib.shape_predictor(
				paths_factory.shape_predictor_5_face_landmarks_path()
			)

			if not os.path.isfile(
				paths_factory.dlib_face_recognition_resnet_model_v1_path()
			):
				syslog.syslog(syslog.LOG_ERR, "DLib Face-Encoder model does not exist")
				error_message()
			self.face_encoder = dlib.face_recognition_model_v1(
				paths_factory.dlib_face_recognition_resnet_model_v1_path()
			)

			if self.use_micromotion_detection:
				from . import dlib_face
				self.micromotion_detector = dlib_face.MicroMotionDetector()

		if self.use_texture_detection or self.use_similarity_detection:
			from . import anti_spoof_core
			if self.use_texture_detection:
				self.texture_detector = anti_spoof_core.TextureDetector()
			if self.use_similarity_detection:
				self.similarity_detector = anti_spoof_core.SimilarityDetector()
		# fmt: on

	def authenticate(self, username, args, cancel_event, timeout):
		# Returns Success, NoMatch, Timeout, NoDevice, Cancelled or Error.

		if not self._initialized:
			return HowdyStatus.NotInitialized

		# Only one instance can use the camera at the same time.
		with self._camera_lock:
			if cancel_event.is_set():
				return HowdyStatus.Cancelled

			try:
				self._cleaned_up = False

				args = self._parse_args(args, timeout.limit)
				if self.micromotion_detector is not None:
					self.micromotion_detector.init(
						self.micromotion_detection_buffer_size
					)

				if self.texture_detector is not None:
					self.texture_detector.init(self.texture_detection_buffer_size)

				if self.similarity_detector is not None:
					self.similarity_detector.init(self.similarity_detection_buffer_size)

				if args.auth_popup:
					try:
						self.ui_uid = pwd.getpwnam(username).pw_uid
						# self.auth_ui.Show(self.ui_uid, S("Starting up..."))
						self.auth_ui.Show(self.ui_uid, S("Face authentication..."))
					except Exception:
						self.ui_uid = None
						syslog.syslog(
							syslog.LOG_ERR,
							"Howdy authentication popup could not be created",
						)
						syslog.syslog(syslog.LOG_ERR, traceback.format_exc())

				try:
					self.video_capture = VideoCapture(self.config)
				except NoDeviceException as exc:
					syslog.syslog(syslog.LOG_ERR, exc.message)
					return HowdyStatus.NoDevice

				return self._authenticate(username, args, cancel_event, timeout)
			except Exception:
				syslog.syslog(
					syslog.LOG_ERR, "Howdy authentication failed with exception"
				)
				syslog.syslog(syslog.LOG_ERR, traceback.format_exc())
				return HowdyStatus.Error
			finally:
				# Release the camera and terminate the auth UI if still running
				if not self._cleaned_up:
					self._cleaned_up = True

					if self.micromotion_detector is not None:
						self.micromotion_detector.reset()

					if self.texture_detector is not None:
						self.texture_detector.reset()

					if self.similarity_detector is not None:
						self.similarity_detector.reset()

					# Release the camera if we opened one (safe if missing/already released)
					if self.video_capture is not None:
						try:
							self.video_capture.release()
						except Exception:
							pass
						finally:
							self.video_capture = None

					# Exit the auth ui process if there is one
					if self.ui_uid is not None:
						self.auth_ui.Close(self.ui_uid)
						self.ui_uid = None

	def _keep_popup_alive(self):
		if self.ui_uid is not None:
			try:
				self.auth_ui.keep_alive(self.ui_uid)
			except Exception:
				pass

	def _prune_snapshots(self):
		try:
			snapshot.prune(self.snapshot_max_age, self.snapshot_max_count)
		except Exception as exc:
			syslog.syslog(
				syslog.LOG_WARNING, f"Could not delete the old snapshots: {exc}"
			)

	def _make_snapshot(self, snapframes, kind, scan_time, frames, lowest_certainty):
		"""Save the first frames of the scan with some metadata, see [snapshots] in
		config.ini. A snapshot that can't be saved must never change the result of
		the authentication."""
		try:
			snapshot.generate(
				snapframes,
				[
					kind + S(" LOGIN"),
					S("Date: ") + datetime.now(UTC).strftime("%Y/%m/%d %H:%M:%S UTC"),
					S("Scan time: ") + str(round(scan_time, 2)) + "s",
					S("Frames: ")
					+ str(frames)
					+ " ("
					+ str(round(frames / max(scan_time, 1e-9), 2))
					+ "FPS)",
					S("Hostname: ") + os.uname().nodename,
					S("Best certainty value: ") + str(round(lowest_certainty * 10, 1)),
				],
			)
			snapshot.prune(self.snapshot_max_age, self.snapshot_max_count)
		except Exception as exc:
			syslog.syslog(syslog.LOG_ERR, f"Could not save the snapshot: {exc}")

	def _authenticate(self, username, args, cancel_event, timeout):
		def send_to_ui(message=None, subtext=None, frame_counter=False):
			# Send message to the auth ui

			# Only execute if the uid is defined (which means the popup was called)
			if self.ui_uid is not None:
				# Try to send the message to the auth ui, but it's okay if that fails
				try:
					if message is not None:
						self.auth_ui.update_message(self.ui_uid, message)
					if subtext is not None:
						self.auth_ui.update_subtext(self.ui_uid, subtext)
					if frame_counter:
						# Form a string to let the user know we're real busy
						subtext = f"Scanned {valid_frames - dark_tries} frames"
						if dark_tries > 1:
							subtext += f" (skipped {dark_tries} dark frames)"
						# Show it in the ui as subtext
						self.auth_ui.update_subtext(self.ui_uid, subtext)
				except Exception:
					pass

		# The model file contents
		models = []
		# Encoded face models
		encodings = []
		# Amount of ignored 100% black frames
		black_tries = 0
		# Amount of ignored dark frames
		dark_tries = 0
		dark_running_total = 0
		# Total amount of frames captured
		frames = 0
		valid_frames = 0
		# Frames whose best match already fell inside the threshold. One frame under
		# the bar is a weak signal: a scan produces dozens of independent looks, so
		# the frame that decides authentication is the *minimum* of many draws, and a
		# single blurred or oddly lit outlier can carry a stranger through. Demanding
		# several agreeing frames removes that outlier without moving the threshold.
		confirmations_seen = 0
		# The first frames of the scan for the snapshot, if enabled
		snapframes = []
		# The lowest certainty value (distance) of all frames, for the snapshot
		lowest_certainty = 10

		# send_to_ui(message=S("Starting up..."))

		# Try to load the face model from the models folder
		try:
			# The models decide who is let in, so only a file that nobody else can change is used
			models = json.loads(
				read_model_file(paths_factory.user_model_path(username), username)
			)
			encoding_size = 256 if self.use_openvino else 128
			encodings, _ = flatten_models(models, encoding_size)
		except FileNotFoundError:
			return HowdyStatus.NoEnrolledFaces

		# Check if the file contains a model
		if len(encodings) < 1:
			return HowdyStatus.NoEnrolledFaces

		if self.use_openvino:
			encodings = np.array(encodings)

		# Fetch the max frame height
		max_height = self.config.getfloat("video", "max_height", fallback=320.0)

		# Get the height of the image (which would be the width if screen is portrait oriented)
		height = self.video_capture.internal.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1
		if self.rotate >= 2:
			height = self.video_capture.internal.get(cv2.CAP_PROP_FRAME_WIDTH) or 1
		# Calculate the amount the image has to shrink
		scaling_factor = (max_height / height) or 1

		# Initiate histogram equalization
		clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

		# Let the ui know that we're ready
		# send_to_ui(message=S("Identifying you..."))
		# send_to_ui(message=S("Face authentication..."))

		# Start the read loop
		start_time = time.monotonic()
		while timeout.time > time.monotonic():
			if cancel_event.is_set():
				return HowdyStatus.Cancelled

			# Increment the frame count every loop
			frames += 1

			send_to_ui(frame_counter=True)
			# Also when the text stays the same, with black frames for example
			self._keep_popup_alive()

			# Stop if we've exceeded the time limit
			if time.monotonic() - start_time > args.timeout:
				# Create a timeout snapshot if enabled
				if self.save_failed:
					self._make_snapshot(
						snapframes,
						S("FAILED"),
						time.monotonic() - start_time,
						frames,
						lowest_certainty,
					)

				if dark_tries == valid_frames:
					syslog.syslog(
						syslog.LOG_ERR,
						S(
							"All frames were too dark, please check dark_threshold in config\n"
							f"Average darkness: {dark_running_total / max(1, valid_frames)}, "
							f"Threshold: {self.dark_threshold}"
						),
					)
					return HowdyStatus.TooDark
				else:
					return HowdyStatus.Timeout

			# Grab a single frame of video
			try:
				frame, gsframe = self.video_capture.read_frame()
			except NoDeviceException as exc:
				syslog.syslog(syslog.LOG_ERR, exc.message)
				return HowdyStatus.Error

			# If snapshots have been turned on, start capturing frames for the snapshot
			if (self.save_failed or self.save_successful) and len(snapframes) < 3:
				snapframes.append(frame.copy())

			gsframe = clahe.apply(gsframe)

			# Create a histogram of the image with 8 values
			hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256])
			# All values combined for percentage calculation
			hist_total = np.sum(hist)

			# Calculate frame darkness
			darkness = hist[0] / hist_total * 100

			# If the image is fully black due to a bad camera read,
			# skip to the next frame
			if (hist_total == 0) or (darkness == 100):
				black_tries += 1
				continue

			dark_running_total += darkness
			valid_frames += 1

			# If the image exceeds darkness threshold due to subject distance,
			# skip to the next frame
			if darkness > self.dark_threshold:
				dark_tries += 1
				continue

			# If the height is too high
			if scaling_factor != 1:
				# Apply that factor to the frame
				frame = cv2.resize(
					frame,
					None,
					fx=scaling_factor,
					fy=scaling_factor,
					interpolation=cv2.INTER_AREA,
				)
				gsframe = cv2.resize(
					gsframe,
					None,
					fx=scaling_factor,
					fy=scaling_factor,
					interpolation=cv2.INTER_AREA,
				)

			# Check portrait in addition to landscape (1), portrait only (2) or
			# turn the frame by a fixed amount (3, 4)
			frame = rotate_frame(frame, self.rotate, frames)
			gsframe = rotate_frame(gsframe, self.rotate, frames)

			if (
				self.spoof_detector is not None
				and self.spoof_detector(frame) > self.anti_spoof_threshold
			):
				continue

			# Get all faces from that frame as encodings
			# Upsamples 1 time
			face_locations = self.face_detector(gsframe, 1)

			if len(face_locations) == 0:
				continue
			if self.use_cnn and not self.use_openvino:
				face_locations = [fl.rect for fl in face_locations]

			def describe(face, frame=frame):
				"""The encoding and, with dlib, the landmarks of a face, how close it
				is to the closest model and the similarity to the most similar one with
				OpenVINO. None without an encoding."""
				if self.use_openvino:
					encoding = self.face_encoder.encode(frame, face)
					if encoding is None:
						return None
					return encoding, None, np.dot(encodings, encoding).max()
				landmarks = self.pose_predictor(frame, face)
				encoding = np.array(
					self.face_encoder.compute_face_descriptor(frame, landmarks, 1)
				)
				return (
					encoding,
					landmarks,
					np.linalg.norm(encodings - encoding, axis=1).min(),
				)

			# One face per frame, the one that is most like the models: the liveness
			# checks compare a frame with the ones before, which only says something
			# about one and the same person
			fl = face_locations[0]
			# Only computed here if there is a choice to make, the checks may reject
			# the face before
			description = None
			if len(face_locations) > 1:
				candidates = [(face, describe(face)) for face in face_locations]
				candidates = [c for c in candidates if c[1] is not None]
				if not candidates:
					continue
				pick = max if self.use_openvino else min
				fl, description = pick(candidates, key=lambda c: c[1][2])

			if (
				self.spoof_detector_flir is not None
				and self.spoof_detector_flir(frame, fl) < self.damo_flir_threshold
			):
				continue

			if (
				self.texture_detector is not None
				and self.texture_detector(gsframe, fl) < self.texture_threshold
			):
				continue

			if self.micromotion_detector is not None:
				if self.use_openvino:
					movement = self.micromotion_detector(frame, fl)
				else:
					shape = (
						self.pose_predictor(frame, fl)
						if description is None
						else description[1]
					)
					movement = self.micromotion_detector(shape)
				if movement < self.micromotion_threshold:
					continue

			if description is None:
				description = describe(fl)
				if description is None:
					continue
			face_encoding, _, best = description

			if self.use_openvino:
				# OpenVINO matches are a similarity, the lowest certainty is a distance
				lowest_certainty = min(lowest_certainty, 1 - best)
				accept = self.certainty < best < 1
			else:
				# Update certainty if we have a new low
				lowest_certainty = min(lowest_certainty, best)
				accept = 0 < best < self.certainty

			if self.similarity_detector is not None:
				similarity, stale = self.similarity_detector(
					face_encoding, self.identical_threshold
				)
				if stale or similarity < self.sudden_change_threshold:
					continue

			# Check if a match that's confident enough
			if accept:
				confirmations_seen += 1
				if confirmations_seen < self.confirmations:
					# Keep scanning until enough separate frames agree
					continue

				# Resend the frame info once more before we terminate
				send_to_ui(frame_counter=True)

				# Make snapshot if enabled
				if self.save_successful:
					self._make_snapshot(
						snapframes,
						S("SUCCESSFUL"),
						time.monotonic() - start_time,
						frames,
						lowest_certainty,
					)

				# Run the rubberstamps if enabled, they can still deny the authentication
				if self.config.getboolean("rubberstamps", "enabled", fallback=False):
					from . import rubberstamps  # type: ignore

					send_to_ui(subtext="")
					try:
						stamps_passed = rubberstamps.execute(
							self.config,
							lambda message=None, subtext=None: send_to_ui(
								message=message, subtext=subtext
							),
							{
								"video_capture": self.video_capture,
								"face_detector": self.face_detector,
								"pose_predictor": self.pose_predictor,
								"clahe": clahe,
							},
							cancel_event,
							keep_alive=self._keep_popup_alive,
							# The client has gone, nobody waits for the result
							abandoned=lambda: timeout.time <= time.monotonic(),
						)
					except rubberstamps.StampCancelled:
						if cancel_event.is_set():
							return HowdyStatus.Cancelled
						return HowdyStatus.KeepAliveTimeout

					if not stamps_passed:
						return HowdyStatus.NoMatch

				# End peacefully
				return HowdyStatus.Success
		return HowdyStatus.KeepAliveTimeout


class Timeout:
	def __init__(self, timeout, limit=False):
		self.time = timeout
		# The caller has no privileges, the timeout it asks for is limited
		self.limit = limit


# The sender of the method call that is handled. dbus-next does not tell the
# methods who calls, remember_sender() (a message handler of the bus) keeps it.
current_sender = contextvars.ContextVar("current_sender", default=None)


def remember_sender(msg):
	if msg.message_type == MessageType.METHOD_CALL:
		current_sender.set(msg.sender)
	return None


class AuthenticationInterface(ServiceInterface):
	def __init__(self, ui_service, bus):
		super().__init__(AUTHENTICATION_INTERFACE)

		self.bus = bus

		self.authenticator = HowdyAuthenticator(ui_service)
		self.executor = ThreadPoolExecutor(
			max_workers=1,
			thread_name_prefix="howdy-auth",
		)

		self.lock = threading.Lock()
		self.requests = {}
		self.active_request = None
		self.cooldown_until = {}
		self.busy_callers = set()

	async def caller_uid(self):
		"""The uid of the caller, identified by the bus and not by what it claims"""
		try:
			return await query_caller_uid(self.bus, current_sender.get())
		except CallerError as exc:
			raise DBusError("org.howdy.Authentication.NotAuthorized", str(exc)) from exc

	@method()
	async def StartAuthentication(self, username: "s", args: "as") -> "s":  # type: ignore
		try:
			target_uid = pwd.getpwnam(username).pw_uid
		except KeyError:
			raise DBusError(
				"org.howdy.Authentication.InvalidUser",
				"Unknown user",
			)

		caller_uid = await self.caller_uid()
		trusted = is_privileged(caller_uid, True)
		if not trusted and target_uid != caller_uid:
			raise DBusError(
				"org.howdy.Authentication.NotAuthorized",
				"Only the own user can be authenticated",
			)

		# Skip repeated model reads for callers already rejected during an active
		# authentication, and for callers in cooldown.
		with self.lock:
			if (
				not trusted
				and self.active_request is not None
				and caller_uid in self.busy_callers
			):
				raise DBusError(
					"org.howdy.Authentication.Busy",
					"An authentication is already running",
				)
			if not trusted and time.monotonic() < self.cooldown_until.get(
				caller_uid, 0
			):
				raise DBusError(
					"org.howdy.Authentication.Busy",
					"Too many authentications, try again in a moment",
				)

		# A user without a face model is answered at once, without the camera and
		# whatever else is running. The client must not tell it from a scan that is
		# about to start, it would ask the user to look into the camera.
		has_model = self.authenticator.has_face_model(username)

		with self.lock:
			if has_model and self.active_request is not None:
				if not trusted:
					self.busy_callers.add(caller_uid)
				raise DBusError(
					"org.howdy.Authentication.Busy",
					"An authentication is already running",
				)
			if not trusted and time.monotonic() < self.cooldown_until.get(
				caller_uid, 0
			):
				raise DBusError(
					"org.howdy.Authentication.Busy",
					"Too many authentications, try again in a moment",
				)

			timeout = Timeout(time.monotonic() + 2, limit=not trusted)
			request_id = str(uuid.uuid4())
			cancel_event = threading.Event()

			if not has_model:
				self.requests[request_id] = {
					"owner": caller_uid,
					"status": HowdyStatus.NoEnrolledFaces,
					"cancel": cancel_event,
					"timeout": timeout,
				}
				# Nobody can use this to keep asking, either
				if not trusted and self.authenticator.start_cooldown > 0:
					self.cooldown_until[caller_uid] = (
						time.monotonic() + self.authenticator.start_cooldown
					)
				self._clear_later(request_id)
				return request_id

			self.requests[request_id] = {
				"owner": caller_uid,
				"status": HowdyStatus.Running,
				"cancel": cancel_event,
				"timeout": timeout,
			}
			self.busy_callers.clear()
			self.active_request = request_id

		def run_authentication():
			try:
				result = self.authenticator.authenticate(
					username, args, cancel_event, timeout
				)
			except Exception:
				syslog.syslog(syslog.LOG_ERR, "Authentication worker failed")
				syslog.syslog(syslog.LOG_ERR, traceback.format_exc())
				result = HowdyStatus.Error

			with self.lock:
				request = self.requests.get(request_id)

				if request is not None:
					# Cancellation takes precedence over a late result.
					if cancel_event.is_set():
						request["status"] = HowdyStatus.Cancelled
					elif isinstance(result, HowdyStatus):
						request["status"] = result
					else:
						request["status"] = HowdyStatus.Error

				if not trusted and self.authenticator.start_cooldown > 0:
					now = time.monotonic()
					self.cooldown_until[caller_uid] = (
						now + self.authenticator.start_cooldown
					)
					self.cooldown_until = {
						uid: until
						for uid, until in self.cooldown_until.items()
						if until > now
					}

				if self.active_request == request_id:
					self.active_request = None
					self.busy_callers.clear()

			self._clear_later(request_id)

		self.executor.submit(run_authentication)
		return request_id

	def _clear_later(self, request_id):
		"""Forget a finished request after a while"""

		def cleanup_request():
			time.sleep(30)
			self.clear(request_id)

		thread = threading.Thread(
			target=cleanup_request,
			name="howdy-auth-cleanup",
			daemon=True,
		)
		thread.start()

	def owns(self, request, caller_uid):
		return may_access(caller_uid, request["owner"], True)

	@method()
	async def GetResult(self, request_id: "s") -> "s":  # type: ignore
		caller_uid = await self.caller_uid()
		with self.lock:
			request = self.requests.get(request_id)

			# A request of somebody else does not exist for the caller
			if request is None or not self.owns(request, caller_uid):
				raise DBusError(
					"org.howdy.Authentication.UnknownRequest",
					"Unknown authentication request",
				)

			request["timeout"].time = time.monotonic() + 1
			# syslog.syslog(
			# 	syslog.LOG_INFO,
			# 	f"GetResult - request_id: {request_id}, object_id: {id(request)}",
			# )
			return str(request["status"])

	@method()
	async def CancelAuthentication(self, request_id: "s") -> "b":  # type: ignore
		caller_uid = await self.caller_uid()
		with self.lock:
			request = self.requests.get(request_id)

			if request is None or not self.owns(request, caller_uid):
				return False

			if request["status"] != HowdyStatus.Running:
				return False

			request["status"] = HowdyStatus.Cancelling
			request["cancel"].set()
			return True

	@method()
	async def ClearAuthentication(self, request_id: "s") -> "b":  # type: ignore
		return self.clear(request_id, await self.caller_uid())

	def clear(self, request_id, caller_uid=None):
		"""Without a caller (the cleanup of old requests) every request can be cleared"""
		with self.lock:
			request = self.requests.get(request_id)
			if request is None or (
				caller_uid is not None and not self.owns(request, caller_uid)
			):
				return False
			del self.requests[request_id]

			if request["status"] == HowdyStatus.Running:
				request["status"] = HowdyStatus.Cancelling
				request["cancel"].set()
			return True

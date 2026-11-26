import os
import sys

os.environ["OPENVINO_TELEMETRY_ENABLE"] = "0"


# fmt: off
# =====================================================================
# Fake the telemetry import to remove telemetry
# =====================================================================
class DummyTelemetry:
	def __init__(self, *args, **kwargs): pass
	def init(self, *args, **kwargs): pass
	def send_event(self, *args, **kwargs): pass
	def start_session(self, *args, **kwargs): pass
	def end_session(self, *args, **kwargs): pass
	def force_shutdown(self, *args, **kwargs): pass
# fmt: on


# Inject an empty module
import types

dummy_mod = types.ModuleType("openvino_telemetry")
dummy_mod.Telemetry = DummyTelemetry

sys.modules["openvino_telemetry"] = dummy_mod
# =====================================================================


import cv2
import pwd
import dlib
import math

import numpy as np

from . import paths_factory  # type: ignore
from .anti_spoof_core import AbstractMicroMotionDetector


DEFAULT_DEVICE = "GPU"

MODEL_DIR = paths_factory.openvino_data_dir_path()
DET_XML = os.path.join(MODEL_DIR, "face-detection-adas-0001.xml")
REID_XML = os.path.join(MODEL_DIR, "face-reidentification-retail-0095.xml")
SPOOF_XML = os.path.join(MODEL_DIR, "anti-spoofing-480.xml")
DAMO_FLIR_XML = os.path.join(MODEL_DIR, "damo-flir.xml")
LANDMARK_MODEL_XML = os.path.join(MODEL_DIR, "facial-landmarks-35-adas-0002.xml")
CACHE_DIR = paths_factory.openvino_cache_dir_path()


_core = None


def _cache_dir():
	"""The cache of the compiled models. Root keeps to the system wide one: what is below a
	home is under the control of its user, who could put a link or files there for root to
	follow or load. Everybody else can not write there and gets one in the own home."""
	if os.geteuid() == 0:
		return str(CACHE_DIR)
	home = pwd.getpwuid(os.geteuid()).pw_dir
	return os.path.join(home, ".cache", "howdy", "openvino")


def _get_core(user=None):
	global _core
	if _core is None:
		try:
			cache_dir = _cache_dir()
			# The OpenCL kernel caches go there too, not into /tmp, where everybody can
			# create the directory first
			for variable, name in (
				("CL_CACHE_DIR", "cl_cache"),
				("INTEL_OCL_CACHE_DIR", "intel_ocl"),
			):
				os.makedirs(os.path.join(cache_dir, name), mode=0o700, exist_ok=True)
				os.environ.setdefault(variable, os.path.join(cache_dir, name))

			from openvino import Core

			_core = Core()
			os.makedirs(cache_dir, mode=0o700, exist_ok=True)
			_core.set_property({"CACHE_DIR": cache_dir})
		except Exception as exc:
			raise Exception(f"Could not load OpenVINO Core cause of: [{exc}]")
	return _core


class OpenVINOException(Exception):
	def __init__(self, *args: object):
		self.message = None
		if len(args) > 0:
			self.message = args[0]
		super().__init__(args)


class FaceDetector:
	def __init__(self, device=DEFAULT_DEVICE, user=None):
		if not os.path.isfile(DET_XML):
			raise OpenVINOException("OpenVINO Face-Detector model does not exist")

		core = _get_core(user=user)
		self.compiled = core.compile_model(core.read_model(DET_XML), device)
		self.input_layer = self.compiled.input(0)
		self.output_layer = self.compiled.output(0)
		self.net_h = self.input_layer.shape[2]
		self.net_w = self.input_layer.shape[3]

	def __call__(self, frame, upsample=None):
		h, w = frame.shape[:2]
		if len(frame.shape) == 2:
			blob_frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
		else:
			blob_frame = frame
		blob = cv2.dnn.blobFromImage(
			blob_frame, size=(self.net_w, self.net_h), ddepth=cv2.CV_8U
		)
		results = self.compiled([blob])[self.output_layer]
		detections = []
		for det in results[0][0]:
			if det[2] > 0.5:
				x1 = max(0, int(det[3] * w))
				y1 = max(0, int(det[4] * h))
				x2 = min(w, int(det[5] * w))
				y2 = min(h, int(det[6] * h))
				detections.append(dlib.rectangle(x1, y1, x2, y2))
		return detections


class FaceEncoder:
	def __init__(self, device=DEFAULT_DEVICE, user=None):
		if not os.path.isfile(REID_XML):
			raise OpenVINOException("OpenVINO Face-Encoder model does not exist")

		core = _get_core(user=user)
		self.compiled = core.compile_model(core.read_model(REID_XML), device)
		self.input_layer = self.compiled.input(0)
		self.output_layer = self.compiled.output(0)
		self.net_h = self.input_layer.shape[2]
		self.net_w = self.input_layer.shape[3]

	def encode(self, frame, face_rect):
		if len(frame.shape) == 2:
			frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
		x1 = max(0, face_rect.left())
		y1 = max(0, face_rect.top())
		x2 = min(frame.shape[1], face_rect.right())
		y2 = min(frame.shape[0], face_rect.bottom())
		face_crop = frame[y1:y2, x1:x2]
		if face_crop.size == 0:
			return None
		blob = cv2.dnn.blobFromImage(
			face_crop, size=(self.net_w, self.net_h), ddepth=cv2.CV_8U
		)
		embedding = self.compiled([blob])[self.output_layer].flatten()
		norm = np.linalg.norm(embedding)
		if norm > 0:
			embedding /= norm
		return embedding


class SpoofDetector:
	def __init__(self, device=DEFAULT_DEVICE, user=None):
		if not os.path.isfile(SPOOF_XML):
			raise OpenVINOException("OpenVINO Spoof-Detector model does not exist")

		core = _get_core(user=user)
		self.compiled = core.compile_model(core.read_model(SPOOF_XML), device)

		self.input_layer = self.compiled.input(0)
		self.output_layer = self.compiled.output(0)

	@staticmethod
	def _sigmoid(x):
		return 1.0 / (
			1.0 + math.exp(-x / 1.25)
		)  # TODO Tuning [1.25 can be used to adjust the certainty spread]

	def __call__(self, frame):
		height, width = frame.shape[:2]

		# Centered square crop with the length of the shorter side, so landscape,
		# portrait (rotate = 1/2) and square frames are all handled the same way.
		side = min(height, width)
		x0 = (width - side) // 2
		y0 = (height - side) // 2

		cv2_im = cv2.resize(
			frame[y0 : y0 + side, x0 : x0 + side],
			(480, 480),
			interpolation=cv2.INTER_LINEAR,
		)

		img = np.transpose(
			(
				cv2_im.astype(np.float32) / 255.0
				- np.array([0.485, 0.456, 0.406], dtype=np.float32)
			)
			/ np.array([0.229, 0.224, 0.225], dtype=np.float32),
			(2, 0, 1),
		)[None, ...]

		return self._sigmoid(
			self.compiled({self.input_layer: img})[self.output_layer][0][0]
		)


class SpoofDetectorFlir:
	def __init__(self, device=DEFAULT_DEVICE, user=None):
		if not os.path.isfile(DAMO_FLIR_XML):
			raise OpenVINOException("OpenVINO Damo-Flir model does not exist")

		core = _get_core(user=user)
		self.compiled = core.compile_model(core.read_model(DAMO_FLIR_XML), device)

		self.input_layer = self.compiled.input(0)
		self.output_layer = self.compiled.output(0)

		shape = self.input_layer.get_partial_shape()
		if shape.rank.is_static and shape.rank.get_length() == 4:
			dims = list(shape)
			self.channels = int(dims[1].get_length()) if dims[1].is_static else 1
			self.height = int(dims[2].get_length()) if dims[2].is_static else 112
			self.width = int(dims[3].get_length()) if dims[3].is_static else 112
		else:
			raise ValueError(f"Expected NCHW input, got {shape}")

		if self.width != 112 or self.height != 112:
			raise ValueError(
				"DAMO FLIR preprocessing produces 112x112, "
				f"but model expects {self.width}x{self.height}"
			)

		if self.channels not in (1, 3):
			raise ValueError(f"Unsupported input channel count: {self.channels}")

	@staticmethod
	def preprocess(frame, rect):
		if frame is None:
			return None

		if hasattr(rect, "left"):
			bbox = [
				rect.left(),
				rect.top(),
				rect.right(),
				rect.bottom(),
			]
		else:
			bbox = list(map(int, rect[:4]))

		img = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)[:, :, 0:3]

		# ModelScope: BGR -> RGB.
		# img = img[..., ::-1]

		h, w = img.shape[:2]
		x1, y1, x2, y2 = map(int, bbox)

		box_w = x2 - x1 + 1
		box_h = y2 - y1 + 1

		x1 = max(0, x1 - int(box_w * 16 / 112))
		x2 = min(w - 1, x2 + int(box_w * 16 / 112))
		y1 = max(0, y1 - int(box_h * 16 / 112))
		y2 = min(h - 1, y2 + int(box_h * 16 / 112))

		padding_h = y2 - y1 + 1
		padding_w = x2 - x1 + 1

		if padding_w > padding_h:
			offset = int((padding_w - padding_h) / 2)
			y1 -= offset
			y2 = y1 + padding_w - 1
			y1 = max(0, y1)
			y2 = min(h - 1, y2)
			dst_size = padding_w
		else:
			offset = int((padding_h - padding_w) / 2)
			x1 -= offset
			x2 = x1 + padding_h - 1
			x1 = max(0, x1)
			x2 = min(w - 1, x2)
			dst_size = padding_h

		dst = np.full((dst_size, dst_size, 3), 127, dtype=np.uint8)

		dst_x_offset = int((dst_size - (x2 - x1 + 1)) / 2)
		dst_y_offset = int((dst_size - (y2 - y1 + 1)) / 2)

		ys = dst_y_offset
		ye = dst_y_offset + y2 + 1 - y1
		xs = dst_x_offset
		xe = dst_x_offset + x2 + 1 - x1

		dst[ys:ye, xs:xe, :] = img[y1 : y2 + 1, x1 : x2 + 1, :]

		img = (
			cv2.resize(dst, (128, 128), interpolation=cv2.INTER_LINEAR)[
				8:120, 8:120, :
			].astype(np.float32)
			- 127.5
		) * 0.0078125

		return np.ascontiguousarray(img.transpose(2, 0, 1)[None, ...])

	@staticmethod
	def _scores(output):
		values = np.asarray(output, dtype=np.float32).reshape(-1)

		if values.size == 1:
			# A single output's meaning depends on the exported model.
			raise ValueError(
				"Model returned one value; verify whether it is "
				"a spoof logit, probability, or another score."
			)
		if values.size != 2:
			raise ValueError(f"Expected two class scores, got shape {values.shape}")

		# Preserve probabilities if already normalized; otherwise
		# treat the values as logits and apply softmax.
		if (
			np.all(values >= 0)
			and np.all(values <= 1)
			and np.isclose(values.sum(), 1.0, atol=1e-3)
		):
			return values
		else:
			values = values / 2.0  # TODO Tuning
			exp = np.exp(values - np.max(values))
			return exp / exp.sum()

	def __call__(self, frame, face_location):
		tensor = self.preprocess(frame, face_location)
		if tensor is None:
			return None

		# DAMO FLIR: higher score means more likely to be spoof.
		# class 0 is spoof, class 1 is real
		return float(
			self._scores(self.compiled({self.input_layer: tensor})[self.output_layer])[
				1
			]
		)


# Idea and code derived from https://github.com/oktorio/liveness_app/blob/main/liveness_app.py
class MicroMotionDetector(AbstractMicroMotionDetector):
	"""
	OpenVINO facial-landmarks-35-adas-0002 plus movement tracking.

	face_rect:
		(x1, y1, x2, y2), or a dlib rectangle.

	Returns:
		landmarks: (35, 2) coordinates in the original frame
		motion_ok: whether measurable movement occurred
		motion_value: displacement in interocular-distance units
	"""

	def __init__(self, device=DEFAULT_DEVICE, user=None):
		if not os.path.isfile(LANDMARK_MODEL_XML):
			raise OpenVINOException("OpenVINO Landmark-Model model does not exist")

		core = _get_core(user=user)
		self.compiled = core.compile_model(core.read_model(LANDMARK_MODEL_XML), device)
		self.input_layer = self.compiled.input(0)
		self.output_layer = self.compiled.output(0)
		super().__init__()

	@staticmethod
	def _rect_to_xyxy(face_rect):
		if all(hasattr(face_rect, name) for name in ("left", "top", "right", "bottom")):
			return (
				face_rect.left(),
				face_rect.top(),
				face_rect.right(),
				face_rect.bottom(),
			)

		return tuple(map(int, face_rect))

	def _get_landmarks(self, frame, face_rect):
		h, w = frame.shape[:2]
		x1, y1, x2, y2 = self._rect_to_xyxy(face_rect)

		# Clamp the bounding box to the frame.
		x1 = max(0, min(w, x1))
		x2 = max(0, min(w, x2))
		y1 = max(0, min(h, y1))
		y2 = max(0, min(h, y2))

		if x2 <= x1 or y2 <= y1:
			self.buffer.clear()
			return None

		face = frame[y1:y2, x1:x2]

		if face.size == 0:
			self.buffer.clear()
			return None

		# The model expects 3 channels and a 60x60 input.
		# For grayscale IR, all three channels will be identical.
		# OpenCV color frames are BGR; convert to RGB.
		# Open Model Zoo model input: NCHW, float32.
		# No ImageNet normalization here.
		points = np.asarray(
			self.compiled(
				{
					self.input_layer: cv2.resize(
						cv2.cvtColor(face, cv2.COLOR_BGR2RGB),
						(60, 60),
						interpolation=cv2.INTER_LINEAR,
					)
					.transpose(2, 0, 1)[None]
					.astype(np.float32)
				}
			)[self.output_layer],
			dtype=np.float32,
		).reshape(35, 2)

		# Convert normalized face-crop coordinates to frame coordinates.
		points[:, 0] = x1 + points[:, 0] * (x2 - x1)
		points[:, 1] = y1 + points[:, 1] * (y2 - y1)

		return points

	def __call__(self, frame, face_rect):
		return self._calculate_displacement(self._get_landmarks(frame, face_rect))

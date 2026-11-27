import cv2
import numpy as np

from abc import ABC
from collections import deque


class AbstractMicroMotionDetector(ABC):
	def __init__(self):
		self.buffer = None

	def init(self, buffer_size=5):
		if buffer_size < 1:
			raise ValueError("Micromotion buffersize has to be at least 1")
		self.buffer = deque(maxlen=buffer_size)

	def reset(self):
		if self.buffer is not None:
			self.buffer.clear()
		self.buffer = None

	def _calculate_displacement(self, landmarks):
		if self.buffer is None:
			return 0.0

		if landmarks is None:
			self.buffer.clear()
			return 0.0

		# Centers of the two eyes.
		# left_eye = (landmarks[0] + landmarks[1]) * 0.5
		# right_eye = (landmarks[2] + landmarks[3]) * 0.5
		# nose = landmarks[4]

		# right_eye - left_eye
		eye_distance = float(
			np.linalg.norm(
				((landmarks[2] + landmarks[3]) * 0.5)
				- ((landmarks[0] + landmarks[1]) * 0.5)
			)
		)

		if eye_distance < 1.0:
			self.buffer.clear()
			return 0.0

		# Track the nose position, normalized by eye distance.
		self.buffer.append((landmarks[4].copy(), eye_distance))

		if len(self.buffer) < self.buffer.maxlen / 2:
			return 0.0

		positions = np.empty((len(self.buffer), 2), dtype=np.float32)
		scales = np.empty(len(self.buffer), dtype=np.float32)

		for i, (position, scale) in enumerate(self.buffer):
			positions[i] = position
			scales[i] = scale

		return float(
			np.linalg.norm(positions - positions[0], axis=1).max() / np.mean(scales)
		)


class TextureDetector:
	def __init__(self):
		self.buffer = None

	def init(self, buffer_size=5):
		if buffer_size < 1:
			raise ValueError("Texture buffersize has to be at least 1")
		self.buffer = deque(maxlen=buffer_size)

	def reset(self):
		if self.buffer is not None:
			self.buffer.clear()
		self.buffer = None

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

	def __call__(self, frame, face_rect):
		if self.buffer is None:
			return None

		height, width = frame.shape[:2]
		x1, y1, x2, y2 = self._rect_to_xyxy(face_rect)

		# Clamp ROI to image bounds.
		x1 = max(0, min(width, x1))
		x2 = max(0, min(width, x2))
		y1 = max(0, min(height, y1))
		y2 = max(0, min(height, y2))

		if x2 <= x1 or y2 <= y1:
			self.buffer.clear()
			return None

		roi = frame[y1:y2, x1:x2]

		if roi.size == 0:
			self.buffer.clear()
			return None

		# Normalize ROI size so texture statistics are more comparable.
		# 1. Laplacian variance
		laplacian_var = float(
			cv2.Laplacian(
				cv2.resize(roi, (128, 128), interpolation=cv2.INTER_AREA), cv2.CV_64F
			).var()
		)

		# Temporal smoothing of the Laplacian measurement.
		self.buffer.append(laplacian_var)

		return float(np.mean(self.buffer))


# Comparisons that are needed before a face is taken for a still picture, a smaller
# window needs all it has
MIN_STALE_COMPARISONS = 2


class SimilarityDetector:
	def __init__(self):
		self.encodings = None
		self.similarities = None

	def init(self, buffer_size=8):
		if buffer_size < 2:
			raise ValueError("Similarity buffersize has to be at least 2")
		self.encodings = deque(maxlen=buffer_size)
		self.similarities = deque(maxlen=buffer_size - 1)

	def reset(self):
		if self.encodings is not None:
			self.encodings.clear()
		if self.similarities is not None:
			self.similarities.clear()
		self.encodings = None
		self.similarities = None

	def __call__(self, encoding, identical_threshold=0.9995):
		if encoding is None or self.encodings is None or self.similarities is None:
			return 1.0, False

		encoding = np.asarray(encoding, dtype=np.float32).reshape(-1)

		if not np.isfinite(encoding).all():
			return 1.0, False

		norm = float(np.linalg.norm(encoding))

		if norm <= 1e-12:
			return 1.0, False

		encoding /= norm

		# Inkonsistente Embedding-Dimensionen nicht vergleichen.
		if self.encodings and (encoding.shape != self.encodings[-1].shape):
			self.reset()

		similarity = None

		if self.encodings:
			similarity = float(np.dot(self.encodings[-1], encoding))
			self.similarities.append(similarity)

		self.encodings.append(encoding.copy())

		if similarity is None:
			return 1.0, False

		# Über das gesamte Fenster nahezu identische Encodings. Ein einzelner Vergleich
		# sagt wenig, ein ruhig gehaltenes Gesicht kann das für einen Moment sein.
		needed = min(MIN_STALE_COMPARISONS, self.similarities.maxlen)
		stale = len(self.similarities) >= needed and all(
			s >= identical_threshold for s in self.similarities
		)

		return similarity, stale

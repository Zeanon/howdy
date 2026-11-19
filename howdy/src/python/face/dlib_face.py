import numpy as np

from .anti_spoof_core import AbstractMicroMotionDetector


# Idea and code derived from https://github.com/oktorio/liveness_app/blob/main/liveness_app.py
class MicroMotionDetector(AbstractMicroMotionDetector):
	"""
	dlib 5-point facial landmarks plus movement tracking.

	face_rect:
	    (x1, y1, x2, y2), or a dlib rectangle.

	Returns:
	    landmarks: (5, 2) coordinates in the original frame
	    motion_value: displacement in interocular-distance units
	"""

	def _get_landmarks(self, landmarks):
		return np.array(
			[
				(landmarks.part(i).x, landmarks.part(i).y)
				for i in range(landmarks.num_parts)
			],
			dtype=np.float32,
		)

	def __call__(self, landmarks):
		return self._calculate_displacement(self._get_landmarks(landmarks))

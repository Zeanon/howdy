# Rotation of captured frames, see the "rotate" option in config.ini

import cv2


def fixed_rotation(rotate):
	"""
	Only the fixed modes (3 and 4) give a stable orientation. The add and test
	commands use them alone: the alternating modes would flicker or store
	models of different orientations.
	"""
	return rotate if rotate in (3, 4) else 0


def upright_rotations(rotate):
	"""
	The ways a frame may have to be turned for an upright face, as fixed modes (0
	is not turned): one for the fixed modes, all of them for the alternating ones.
	"""
	if rotate == 1:
		return [0, 3, 4]
	if rotate == 2:
		return [3, 4]
	return [fixed_rotation(rotate)]


def applied_rotation(rotate, frame_number):
	"""
	The fixed mode (0 is not turned) that rotate_frame() applies to the frame with
	this number.
	"""
	# Check landscape and both portrait orientations
	if rotate == 1:
		return (0, 3, 4)[frame_number % 3]
	# Check both portrait orientations
	if rotate == 2:
		return 3 if frame_number % 2 == 0 else 4
	# Fixed portrait orientations, for cameras that are mounted sideways
	return fixed_rotation(rotate)


def rotate_frame(frame, rotate, frame_number):
	"""
	Returns the frame turned the way the rotate option asks for. Modes 1 and 2
	try several orientations, so they depend on the frame number.
	"""
	applied = applied_rotation(rotate, frame_number)
	if applied == 3:
		return cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
	if applied == 4:
		return cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE)
	return frame

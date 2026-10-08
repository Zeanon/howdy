# Guided multi-sample face capture used by `ubuntu-hello add`.
#
# A model used to be one 128-d descriptor from the first frame that showed a
# face: one pose, one expression, one IR exposure. Authentication is a nearest
# neighbour distance to a stored descriptor, so that single snapshot often
# landed just past the certainty threshold when the head or the light moved
# a little. Windows Hello enrolls from many frames while guiding the user
# through small head movements; this module does the same. All the camera /
# dlib work is passed in as callables so the sequencing is unit-testable.

import time


def N_(text):
	"""gettext marker: extracted by xgettext, translated by add.py at print time."""
	return text

# Guidance steps: (key, translatable prompt). `key` is the machine-readable id
# printed as `@guide <key>` for the GTK apps; the prompt is for the terminal.
# The steps are small movements the frontal detector still handles.
GUIDE_STEPS = (
	("center", N_("Look straight at the camera")),
	("left", N_("Turn your head slightly to the left")),
	("right", N_("Turn your head slightly to the right")),
	("up", N_("Tilt your chin up a little")),
	("down", N_("Tilt your chin down a little")),
	("center", N_("Look straight at the camera again")),
)

# Seconds the user gets to move after each prompt (frames are read and
# discarded so the capture never sees a stale buffered frame).
SETTLE_SECONDS = 2.8
# Per step: how long to look for usable frames, and how many samples to keep.
STEP_SECONDS = 6.0
SAMPLES_PER_STEP = 2
# Descriptors closer than this (euclidean, same scale as the certainty
# threshold 0.35) add nothing to the matcher and are not stored.
DUPLICATE_DISTANCE = 0.05
# Safety cap on samples per model (auth cost is one norm per stored vector).
MAX_SAMPLES = 1 + len(GUIDE_STEPS) * SAMPLES_PER_STEP


def frame_darkness(gsframe, np, cv2):
	"""Percentage of pixels in the darkest of 8 histogram bins, or None for an empty frame."""
	hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256])
	total = np.sum(hist)
	if total == 0:
		return None
	return float(np.asarray(hist).ravel()[0] / total * 100)


def is_duplicate(descriptor, existing, np, tolerance=DUPLICATE_DISTANCE):
	"""True when `descriptor` is within `tolerance` of any vector in `existing`."""
	if not existing:
		return False
	dists = np.linalg.norm(np.array(existing) - np.array(descriptor), axis=1)
	return bool(np.min(dists) < tolerance)


def guide_line(key):
	return "@guide " + key


def progress_line(count, total):
	return "@progress %d/%d" % (count, total)


def _settle(read_frame, clock, sleep, settle_seconds):
	"""Let the user move into position, keeping the camera pipeline flowing."""
	settle_until = clock() + settle_seconds
	while clock() < settle_until:
		read_frame()
		if sleep is not None:
			sleep(0.01)


def _capture_step(read_frame, detect_faces, encode_face, samples, clock,
				  wanted, deadline, max_samples, on_sample):
	"""Append up to *wanted* new descriptors to *samples* before *deadline*.

	Calls *on_sample* as each one lands so progress stays live during the step.
	Duplicates and frames without exactly one face are skipped.
	"""
	got = 0
	while got < wanted and len(samples) < max_samples and clock() < deadline:
		frame, gsframe = read_frame()
		if gsframe is None:
			continue
		faces = detect_faces(gsframe)
		if len(faces) != 1:
			continue
		descriptor = list(encode_face(frame, faces[0]))
		if is_duplicate(descriptor, samples, _np()):
			continue
		samples.append(descriptor)
		got += 1
		on_sample()


def capture_guided_samples(read_frame, detect_faces, encode_face, emit, first_sample,
                           clock=time.monotonic, sleep=None, steps=GUIDE_STEPS,
                           settle_seconds=SETTLE_SECONDS, step_seconds=STEP_SECONDS,
                           samples_per_step=SAMPLES_PER_STEP, max_samples=MAX_SAMPLES):
	"""Walk the user through `steps`, collecting face descriptors.

	read_frame()             -> (frame, gsframe_or_None); gsframe None = skip (dark/black)
	detect_faces(gsframe)    -> list of face locations
	encode_face(frame, loc)  -> descriptor as a python list
	emit(line)               -> prints one protocol/prompt line
	first_sample             -> descriptor already captured for the initial frontal frame

	Returns the list of descriptors (first_sample first). A step that yields no
	usable frame before its deadline is skipped; enrollment still succeeds
	with whatever was captured, the caller decides on the minimum.
	"""
	samples = [list(first_sample)]
	total = min(max_samples, 1 + len(steps) * samples_per_step)
	emit(progress_line(len(samples), total))

	for key, prompt in steps:
		if len(samples) >= max_samples:
			break
		emit(guide_line(key))
		emit(prompt)

		_settle(read_frame, clock, sleep, settle_seconds)

		_capture_step(read_frame, detect_faces, encode_face, samples, clock,
					  samples_per_step, clock() + step_seconds, max_samples,
					  lambda: emit(progress_line(len(samples), total)))

	return samples


def _np():
	import numpy
	return numpy


def flatten_models(models):
	"""(encodings, owners): every stored descriptor plus the index of the model it came from.

	Models now hold several descriptors each, so the argmin over the flat
	encodings list is no longer a model index; `owners[i]` maps it back.
	"""
	encodings = []
	owners = []
	for index, model in enumerate(models):
		data = model.get("data", [])
		encodings.extend(data)
		owners.extend([index] * len(data))
	return encodings, owners

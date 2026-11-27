# Guided multi-sample face capture used by `howdy add`.
#
# A model used to be one 128-d descriptor from the first frame that showed a
# face: one pose, one expression, one IR exposure. Authentication is a nearest
# neighbour distance to a stored descriptor, so that single snapshot often
# landed just past the certainty threshold when the head or the light moved
# a little. Windows Hello enrolls from many frames while guiding the user
# through small head movements; this module does the same. All the camera /
# dlib work is passed in as callables so the sequencing is unit-testable.

import os
import time
import queue
import threading

from .i18n import S


os.environ["GDK_BACKEND"] = "x11"


# Guidance steps: (key, translatable prompt). `key` is the machine-readable id
# printed as `@guide <key>` for the GTK apps; the prompt is for the terminal.
# The steps are small movements the frontal detector still handles.
GUIDE_STEPS = [
	"Look straight at the camera",
	"Turn your head slightly to the left",
	"Turn your head slightly to the right",
	"Tilt your chin up a little",
	"Tilt your chin down a little",
	"Look straight at the camera again",
]

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
INIT_WAIT_SECONDS = 2
END_WAIT_SECONDS = 1
# How long a cancelled capture is waited for
CAMERA_WAIT_SECONDS = 3

HIGHLIGHT_COLOR = (42, 170, 229)  # 2AAAE5


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


def progress_line(count, total):
	return "@progress %d/%d" % (count, total)


def guide_line(key):
	return "@guide " + key


def capture_guided_samples(
	say,
	use_cnn,
	use_openvino,
	face_encoder,
	face_detector,
	video_capture,
	pose_predictor,
	dark_threshold,
	dialog=None,
	clock=time.monotonic,
	steps=GUIDE_STEPS,
	settle_seconds=SETTLE_SECONDS,
	step_seconds=STEP_SECONDS,
	init_wait_seconds=INIT_WAIT_SECONDS,
	end_wait_seconds=END_WAIT_SECONDS,
	samples_per_step=SAMPLES_PER_STEP,
	max_samples=MAX_SAMPLES,
	ring_color=HIGHLIGHT_COLOR,
	rotate=0,
):
	import gi
	import cv2
	import numpy as np

	gi.require_version("Gtk", "3.0")
	gi.require_version("Gdk", "3.0")
	from gi.repository import Gdk as gdk
	from gi.repository import Gtk as gtk
	from gi.repository import GLib as glib
	from gi.repository import GdkPixbuf as gdkPixbuf

	from . import enroll_helper
	from .rotation import fixed_rotation, rotate_frame
	from .recorders.video_capture import NoDeviceException

	# The camera is shown in a window while it records
	if gdk.Screen.get_default() is None:
		raise Exception(
			S(
				"Adding a model shows the camera in a window, run it in a graphical session"
			)
		)

	rotate = fixed_rotation(rotate)

	# Count the number of illuminated read frames
	valid_frames = 0
	# Count the number of illuminated frames that
	# were rejected for being too dark
	dark_tries = 0
	# Track the running darkness total
	dark_running_total = 0
	# The same prompts go to a desktop notification card: the banner sits right
	# under the camera, where the user is looking during the scan.
	progress = {"count": 0, "total": 0, "prompt": ""}
	events = queue.Queue()
	cancel = threading.Event()
	finished = threading.Event()
	latest_frame = None
	last_overlay = None
	frame_lock = threading.Lock()
	result = {"samples": None, "error": None}

	clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

	"""Same public API; capture runs in a worker while GTK stays responsive."""
	builder = gtk.Builder()
	window = builder.get_object("mainwindow")

	box = gtk.Box(orientation=gtk.Orientation.VERTICAL, spacing=8)
	# box.set_border_width(12)

	inner_box = gtk.Box(orientation=gtk.Orientation.VERTICAL, spacing=8)
	guide_label = gtk.Label()
	guide_label.set_line_wrap(True)
	inner_box.pack_start(guide_label, False, False, 0)

	progress_label = gtk.Label()
	progress_label.get_style_context().add_class("dim-label")
	inner_box.pack_start(progress_label, False, False, 0)

	box.pack_start(inner_box, False, False, 30)

	image = gtk.Image()
	image.set_hexpand(True)
	# image.set_vexpand(True)
	box.pack_start(image, True, True, 0)

	abort_button = gtk.Button(label="Cancel")
	abort_button.set_hexpand(False)

	def abort(*_):
		# With the samples complete only the time they are shown is cut short
		if result["samples"] is None and result["error"] is None:
			result["error"] = enroll_helper.EnrollCancelled()
		cancel.set()
		finished.set()

	abort_button.connect("clicked", abort)
	button_box = gtk.Box(orientation=gtk.Orientation.HORIZONTAL)
	button_box.pack_start(abort_button, True, True, 75)
	box.pack_start(button_box, False, False, 20)

	if dialog is None:
		dialog = enroll_helper.create_enroll_dialog(parent=window)
		content = dialog.get_content_area()
	else:
		content = dialog.get_content_area()
		content.foreach(lambda child: content.remove(child))
	content.add(box)

	def encode(frame, face_location):
		if use_openvino:
			face_encoding = face_encoder.encode(frame, face_location)
			if face_encoding is None:
				raise Exception(S("Failed to encode face, aborting"))
			return face_encoding.tolist()
		else:
			face_landmark = pose_predictor(
				frame, face_location.rect if use_cnn else face_location
			)
			return np.array(
				face_encoder.compute_face_descriptor(frame, face_landmark, 1)
			).tolist()

	def compute_face_locations(overlay):
		nonlocal valid_frames, dark_tries, dark_running_total

		# If the image is fully black due to a bad camera read, skip it
		darkness = frame_darkness(overlay, np, cv2)
		if darkness is None or darkness == 100:
			return [], None

		# Include this frame in calculating our average session brightness
		dark_running_total += darkness
		valid_frames += 1

		# If the image exceeds darkness threshold due to subject distance, skip it
		if darkness > dark_threshold:
			dark_tries += 1
			return [], None

		face_locations = face_detector(overlay, 1)
		overlay = np.zeros((*overlay.shape, 3), dtype=np.uint8)
		for loc in face_locations:
			half_width = (loc.right() - loc.left()) / 2
			half_height = (loc.bottom() - loc.top()) / 2

			# Get the center X and Y from the rectangular points
			x = int(half_width) + loc.left()
			y = int(half_height) + loc.top()

			# Get the radius from the with of the square
			r_horizontal = int(half_width * 1.3)
			r_vertical = int(half_height * 1.3)

			cv2.ellipse(
				overlay, (x, y), (r_horizontal, r_vertical), 0, 0, 360, ring_color, 3
			)  # Thickness 2 or 3?
		return face_locations, overlay

	def get_frame():
		nonlocal latest_frame, last_overlay

		try:
			frame, gsframe = video_capture.read_frame()
		except NoDeviceException as exc:
			# Ends the capture, there is nothing to look for without a picture
			raise Exception(exc.message) from exc

		# Turn sideways mounted cameras upright, so the model is built from the
		# same image that is used for authentication
		frame = rotate_frame(frame, rotate, 0)
		gsframe = rotate_frame(gsframe, rotate, 0)

		gsframe = clahe.apply(gsframe)

		face_locations, overlay = compute_face_locations(gsframe)

		gsframe = cv2.cvtColor(gsframe, cv2.COLOR_GRAY2RGB)
		if overlay is not None:
			alpha = 0.75
			mask = np.any(overlay != 0, axis=2)
			gsframe[mask] = (
				overlay[mask].astype(np.float32) * alpha
				+ gsframe[mask].astype(np.float32) * (1 - alpha)
			).astype(np.uint8)
			# cv2.addWeighted(overlay, alpha, display_frame, 1 - alpha, 0, display_frame)

		gsframe = cv2.flip(gsframe, 1)
		height, width = gsframe.shape[:2]
		data = glib.Bytes.new(gsframe.tobytes())

		with frame_lock:
			latest_frame = (data, width, height)

		return frame, face_locations

	def emit(line):
		nonlocal progress
		"""Print one capture line; mirror prompts / progress onto the notification."""
		if line.startswith("@progress "):
			try:
				count, total = line[len("@progress ") :].split("/", 1)
				progress["count"], progress["total"] = int(count), int(total)
			except ValueError:
				pass
			progress_label.set_markup(
				S(f"<span foreground='#2AAAE5'><b>Samples: {count}/{total}</b></span>")
			)
			return
		if line.startswith("@guide "):
			# if builtins.howdy_args.gui:
			# 	say(line)
			line = line[len("@guide ") :]
			guide_label.set_markup(
				S(f"<big><span foreground='#2AAAE5'><b>{line}</b></span></big>")
			)
		say(S(line))

	def worker():
		try:
			capture()
		except Exception as exc:
			# The first reason counts (the user has cancelled, for example)
			if result["error"] is None:
				result["error"] = exc
		finally:
			# Always, the window waits for it
			finished.set()

	def capture():
		nonlocal valid_frames, dark_tries, dark_running_total

		# Give the user time to read
		deadline = clock() + init_wait_seconds
		while not cancel.is_set() and clock() < deadline:
			get_frame()

		# Loop through frames till we hit a timeout
		face_locations = []
		for _ in range(60):
			if cancel.is_set():
				return

			# Get all faces from that frame as encodings
			frame, face_locations = get_frame()

			# If we've found at least one, we can continue
			if face_locations:
				break

		# If we've found no faces, try to determine why
		if not face_locations:
			# video_capture.release()
			if valid_frames == 0:
				reason = S("Camera saw only black frames - is IR emitter working?")
			elif valid_frames == dark_tries:
				reason = (
					S("All frames were too dark, please check dark_threshold in config")
					+ " "
					+ S(
						f"Average darkness: {dark_running_total / valid_frames}, Threshold: {dark_threshold}"
					)
				)
			else:
				reason = S("No face detected, aborting")
			raise Exception(reason)

		# If more than 1 faces are detected we can't know which one belongs to the user
		elif len(face_locations) > 1:
			# video_capture.release()
			raise Exception(S("Multiple faces detected, aborting"))

		first_sample = encode(frame, face_locations[0])
		samples = [list(first_sample)]
		total = min(max_samples, 1 + len(steps) * samples_per_step)

		events.put(("progress", len(samples), total))

		for prompt in steps:
			if cancel.is_set() or len(samples) >= max_samples:
				break

			events.put(("emit", guide_line(prompt)))

			settle_until = clock() + settle_seconds
			while not cancel.is_set() and clock() < settle_until:
				get_frame()

			deadline = clock() + step_seconds
			got = 0

			while (
				not cancel.is_set()
				and got < samples_per_step
				and len(samples) < max_samples
				and clock() < deadline
			):
				frame, face_locations = get_frame()
				if len(face_locations) != 1:
					continue

				descriptor = list(encode(frame, face_locations[0]))
				if is_duplicate(descriptor, samples, np):
					continue

				samples.append(descriptor)
				got += 1
				events.put(("progress", len(samples), total))

		if cancel.is_set():
			return

		result["samples"] = samples
		# The result is shown for a moment
		events.put(("finished",))
		cancel.wait(end_wait_seconds)

	def update_ui():
		nonlocal latest_frame

		with frame_lock:
			current = latest_frame
			latest_frame = None

		if current is not None:
			data, width, height = current
			pixbuf = gdkPixbuf.Pixbuf.new_from_bytes(
				data,
				gdkPixbuf.Colorspace.RGB,
				False,
				8,
				width,
				height,
				width * 3,
			)
			image.set_from_pixbuf(pixbuf)

		while True:
			try:
				event = events.get_nowait()
			except queue.Empty:
				break

			kind = event[0]

			if kind == "emit":
				emit(event[1])
			elif kind == "progress":
				count, maximum = event[1:]
				emit(progress_line(count, maximum))
			elif kind == "finished":
				progress_label.set_markup(
					f"<span foreground='#2AAAE5'><b>Captured {len(result['samples'])} face samples</b></span>"
				)

		if finished.is_set():
			loop.quit()
			return glib.SOURCE_REMOVE
		return glib.SOURCE_CONTINUE

	dialog.show_all()

	loop = glib.MainLoop()
	glib.timeout_add(33, update_ui)

	# The window is only touched here, in the thread of the UI
	emit(S("\nPlease look straight into the camera"))
	guide_label.set_markup(
		f"<big><span foreground='#2AAAE5'><b>{GUIDE_STEPS[0]}</b></span></big>"
	)

	thread = threading.Thread(
		target=worker,
		name="howdy-enroll",
		daemon=True,
	)
	thread.start()

	# Closing the dialog is cancelling too
	dialog.connect("response", abort)

	try:
		loop.run()
	finally:
		cancel.set()
		dialog.destroy()
		# The caller releases the camera, nothing may read it any more then. A camera
		# that does not answer must not hold up the window for good.
		thread.join(CAMERA_WAIT_SECONDS)
		if thread.is_alive():
			video_capture.still_reading = True

	if result["error"] is not None:
		raise result["error"]

	return result["samples"]


def flatten_models(models, data_size):
	"""(encodings, owners): every stored descriptor plus the index of the model it came from.

	Models now hold several descriptors each, so the argmin over the flat
	encodings list is no longer a model index; `owners[i]` maps it back.
	"""
	encodings = []
	owners = []
	for index, model in enumerate(models):
		data = model.get("data", [])
		if all(len(dat) == data_size for dat in data):
			encodings.extend(data)
			owners.extend([index] * len(data))
	return encodings, owners

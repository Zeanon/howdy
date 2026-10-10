# Guided multi-sample face capture used by `howdy add`.
#
# A model used to be one 128-d descriptor from the first frame that showed a
# face: one pose, one expression, one IR exposure. Authentication is a nearest
# neighbour distance to a stored descriptor, so that single snapshot often
# landed just past the certainty threshold when the head or the light moved
# a little. Windows Hello enrolls from many frames while guiding the user
# through small head movements; this module does the same. All the camera /
# dlib work is passed in as callables so the sequencing is unit-testable.

import time
import gi
import cv2
import queue
import threading
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


def capture_guided_samples(
	read_frame,
	detect_faces,
	encode_face,
	emit,
	first_sample,
	clock=time.monotonic,
	sleep=None,
	steps=GUIDE_STEPS,
	settle_seconds=SETTLE_SECONDS,
	step_seconds=STEP_SECONDS,
	samples_per_step=SAMPLES_PER_STEP,
	max_samples=MAX_SAMPLES,
):
	gi.require_version("Gtk", "3.0")
	from gi.repository import Gtk, Gdk, GLib, GdkPixbuf

	dialog_width = 500
	dialog_height = 545
	
	"""Same public API; capture runs in a worker while GTK stays responsive."""

	samples = [list(first_sample)]
	total = min(max_samples, 1 + len(steps) * samples_per_step)

	builder = Gtk.Builder()
	window = builder.get_object("mainwindow")

	dialog = Gtk.Dialog(
		title="Howdy – Create Model",
		parent=window,
		flags=Gtk.DialogFlags.MODAL
	)
	dialog.set_default_size(dialog_width, dialog_height)
	dialog.set_keep_above(True)
	dialog.set_resizable(False)
	dialog.set_can_focus(False)
	dialog.set_focus(None)
	dialog.set_modal(True)
	dialog.set_decorated(False)
	
	screen = Gdk.Screen.get_default()
	dialog.move((screen.get_width() / 2) - (dialog_width / 2), 0)

	content = dialog.get_content_area()
	box = Gtk.Box(
		orientation=Gtk.Orientation.VERTICAL,
		spacing=8
	)
	# box.set_border_width(12)
	content.add(box)

	inner_box = Gtk.Box(
		orientation=Gtk.Orientation.VERTICAL,
		spacing=8
	)
	prompt_label = Gtk.Label()
	prompt_label.set_line_wrap(True)
	inner_box.pack_start(prompt_label, False, False, 0)

	progress_label = Gtk.Label()
	inner_box.pack_start(progress_label, False, False, 0)

	box.pack_start(inner_box, False, False, 30)

	image = Gtk.Image()
	image.set_hexpand(True)
	# image.set_vexpand(True)
	box.pack_start(image, True, True, 0)

	events = queue.Queue()
	cancel = threading.Event()
	finished = threading.Event()
	latest_frame = [None]
	frame_lock = threading.Lock()
	result = {"samples": None, "error": None}

	def worker():
		import cv2
		import numpy as np

		def publish_frame(gsframe):
			if gsframe is None:
				return

			gsframe = cv2.flip(gsframe, 1)
			if gsframe.ndim == 2:
				rgb = cv2.cvtColor(gsframe, cv2.COLOR_GRAY2RGB)
			else:
				rgb = cv2.cvtColor(gsframe, cv2.COLOR_BGR2RGB)

			height, width, _ = rgb.shape
			data = GLib.Bytes.new(rgb.tobytes())

			with frame_lock:
				latest_frame[0] = (data, width, height)

		try:
			events.put(("progress", len(samples), total))

			for key, prompt in steps:
				if cancel.is_set() or len(samples) >= max_samples:
					break

				events.put(("emit", guide_line(key)))
				events.put(("emit", prompt))
				events.put(("prompt", prompt))

				settle_until = clock() + settle_seconds
				while not cancel.is_set() and clock() < settle_until:
					_, gsframe = read_frame()
					publish_frame(gsframe)
					if sleep:
						sleep(0.01)

				deadline = clock() + step_seconds
				got = 0

				while (
					not cancel.is_set()
					and got < samples_per_step
					and len(samples) < max_samples
					and clock() < deadline
				):
					frame, gsframe = read_frame()
					if gsframe is None:
						continue

					publish_frame(gsframe)

					faces = detect_faces(gsframe)
					if len(faces) != 1:
						continue

					descriptor = list(encode_face(frame, faces[0]))
					if is_duplicate(descriptor, samples, np):
						continue

					samples.append(descriptor)
					got += 1
					events.put(("progress", len(samples), total))

			result["samples"] = samples

		except Exception as exc:
			result["error"] = exc

		finally:
			finished.set()
			events.put(("finished",))

	def update_ui():
		with frame_lock:
			current = latest_frame[0]
			latest_frame[0] = None

		if current is not None:
			data, width, height = current
			pixbuf = GdkPixbuf.Pixbuf.new_from_bytes(
				data,
				GdkPixbuf.Colorspace.RGB,
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
			elif kind == "prompt":
				prompt_label.set_markup(f"<big><span foreground='#2aaae5'><b>{event[1]}</b></span></big>")
			elif kind == "progress":
				_, count, maximum = event
				progress_label.set_markup(f"<span foreground='#2aaae5'><b>Samples: {count}/{maximum}</b></span>")
				emit(progress_line(count, maximum))

		if finished.is_set():
			loop.quit()
			return GLib.SOURCE_REMOVE

		return GLib.SOURCE_CONTINUE

	dialog.show_all()

	loop = GLib.MainLoop()
	GLib.timeout_add(33, update_ui)

	thread = threading.Thread(
		target=worker,
		name="howdy-guided-capture",
		daemon=True,
	)
	thread.start()

	dialog.connect("response", lambda *_: cancel.set())

	try:
		loop.run()
	finally:
		cancel.set()
		dialog.destroy()

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

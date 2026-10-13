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
import sys
import queue
import builtins
import threading
import time
import cairo
import math
import os


os.environ["GDK_BACKEND"] = "x11"


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

HIGHLIGHT_COLOR = (42, 170, 229) #2AAAE5

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
	say,
	use_cnn,
	use_openvino,
	face_encoder,
	face_detector,
	video_capture,
	pose_predictor,
	dark_threshold,
	clock=time.monotonic,
	steps=GUIDE_STEPS,
	settle_seconds=SETTLE_SECONDS,
	step_seconds=STEP_SECONDS,
	samples_per_step=SAMPLES_PER_STEP,
	max_samples=MAX_SAMPLES,
	ring_color=HIGHLIGHT_COLOR,
):
	import gi
	import cv2
	import numpy as np

	gi.require_version("Gtk", "3.0")
	from gi.repository import Gtk as gtk
	from gi.repository import Gdk as gdk
	from gi.repository import GLib as glib
	from gi.repository import GdkPixbuf as gdkPixbuf

	dialog_width = 500
	dialog_height = 545


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
	clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
	
	"""Same public API; capture runs in a worker while GTK stays responsive."""
	builder = gtk.Builder()
	window = builder.get_object("mainwindow")

	dialog = gtk.Dialog(
		title="Howdy – Create Model",
		parent=window,
		flags=gtk.DialogFlags.MODAL
	)
	dialog.connect(
		"draw",
		lambda widget, ctx: draw_background(widget, ctx)
	)

	# dialog.set_type_hint(gdk.WindowTypeHint.NOTIFICATION)
	dialog.set_default_size(dialog_width, dialog_height)
	dialog.set_focus_on_map(builtins.howdy_args.gui)
	dialog.set_app_paintable(True)
	dialog.set_keep_above(True)
	dialog.set_resizable(False)
	dialog.set_can_focus(False)
	dialog.set_decorated(False)
	dialog.set_modal(True)
	
	screen = gdk.Screen.get_default()
	dialog.set_visual(screen.get_rgba_visual())
	dialog.move((screen.get_width() / 2) - (dialog_width / 2), 0)

	content = dialog.get_content_area()
	box = gtk.Box(
		orientation=gtk.Orientation.VERTICAL,
		spacing=8
	)
	# box.set_border_width(12)
	content.add(box)

	inner_box = gtk.Box(
		orientation=gtk.Orientation.VERTICAL,
		spacing=8
	)
	prompt_label = gtk.Label()
	prompt_label.set_line_wrap(True)
	inner_box.pack_start(prompt_label, False, False, 0)

	progress_label = gtk.Label()
	progress_label.get_style_context().add_class("dim-label")
	inner_box.pack_start(progress_label, False, False, 0)

	box.pack_start(inner_box, False, False, 30)

	image = gtk.Image()
	image.set_hexpand(True)
	# image.set_vexpand(True)
	box.pack_start(image, True, True, 0)

	events = queue.Queue()
	cancel = threading.Event()
	finished = threading.Event()
	latest_frame = [None]
	frame_lock = threading.Lock()
	result = {"samples": None, "error": None}


	def draw_background(widget, ctx):
		radius = 18
		border = 2
		half = border / 2

		dialog.get_window().set_cursor(gdk.Cursor(gdk.CursorType.TARGET)) #TODO change?

		# Fensterfläche transparent löschen
		ctx.set_operator(cairo.OPERATOR_SOURCE)
		ctx.set_source_rgba(0, 0, 0, 0)
		ctx.paint()

		ctx.set_operator(cairo.OPERATOR_OVER)
		ctx.new_path()

		# Linke Kante, ohne obere Kante
		ctx.move_to(half, 0)
		ctx.line_to(half, dialog_height - radius - half)

		# Linke untere Rundung
		ctx.arc_negative(
			radius + half,
			dialog_height - radius - half,
			radius,
			math.pi,
			math.pi / 2
		)

		# Untere Kante
		ctx.line_to(dialog_width - radius - half, dialog_height - half)

		# Rechte untere Rundung
		ctx.arc_negative(
			dialog_width - radius - half,
			dialog_height - radius - half,
			radius,
			math.pi / 2,
			0
		)

		# Rechte Kante, ohne obere Kante
		ctx.line_to(dialog_width - half, 0)

    	# Theme-Hintergrundfarbe auslesen
		style = widget.get_style_context()
		found, bg_color = style.lookup_color("theme_bg_color")
		# bg_color = style.get_background_color(gtk.StateFlags.NORMAL)

		if found:
			ctx.set_source_rgba(bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha)
		else:
			# Fallback, falls die Farbe nicht verfügbar ist
			ctx.set_source_rgb(0, 0, 0)
		ctx.fill_preserve()

		ctx.set_line_width(border)
		ctx.set_line_join(cairo.LINE_JOIN_ROUND)
		ctx.set_source_rgb(HIGHLIGHT_COLOR[0]/255, HIGHLIGHT_COLOR[1]/255, HIGHLIGHT_COLOR[2]/255)

		ctx.stroke()

	def encode(frame, face_location):
		if use_openvino:
			face_encoding = face_encoder.encode(frame, face_location)
			if face_encoding is None:
				print("Failed to encode face, aborting")
				sys.exit(1)
			return face_encoding.tolist()
		else:
			face_landmark = pose_predictor(frame, face_location.rect if use_cnn else face_location)
			return np.array(face_encoder.compute_face_descriptor(frame, face_landmark, 1)).tolist()


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
		overlay = cv2.cvtColor(overlay, cv2.COLOR_GRAY2RGB)
		for loc in face_locations:
			half_width = (loc.right() - loc.left()) / 2
			half_height = (loc.bottom() - loc.top()) / 2

			# Get the center X and Y from the rectangular points
			x = int(half_width) + loc.left()
			y = int(half_height) + loc.top()

			# Get the radius from the with of the square
			r_horizontal = int(half_width * 1.2)
			r_vertical = int(half_height * 1.2)

			cv2.ellipse(overlay, (x, y), (r_horizontal, r_vertical), 0, 0, 360, ring_color, 3) #Thickness 2 or 3?

		return face_locations, overlay

	def get_frame():
		frame, gsframe = video_capture.read_frame()
		gsframe = clahe.apply(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY))

		face_locations, overlay = compute_face_locations(gsframe)

		gsframe = cv2.cvtColor(gsframe, cv2.COLOR_GRAY2RGB)
		if overlay is not None:
			alpha = 0.65
			cv2.addWeighted(overlay, alpha, gsframe, 1 - alpha, 0, gsframe)

		gsframe = cv2.flip(gsframe, 1)
		height, width = gsframe.shape[:2]
		data = glib.Bytes.new(gsframe.tobytes())

		with frame_lock:
			latest_frame[0] = (data, width, height)

		return frame, face_locations


	def emit(line):
		nonlocal progress
		"""Print one capture line; mirror prompts / progress onto the notification."""
		if line.startswith("@progress "):
			try:
				count, total = line[len("@progress "):].split("/", 1)
				progress["count"], progress["total"] = int(count), int(total)
			except ValueError:
				pass
			progress_label.set_markup(f"<span foreground='#2AAAE5'><b>Samples: {count}/{total}</b></span>")
			if builtins.howdy_args.gui:
				say(line)
			return
		if line.startswith("@"):
			if builtins.howdy_args.gui:
				say(line)
			return
		prompt_label.set_markup(f"<big><span foreground='#2AAAE5'><b>{N_(line)}</b></span></big>")
		say(N_(line))


	def worker():
		nonlocal valid_frames, dark_tries, dark_running_total

		emit("@guide center")
		say(N_("\nPlease look straight into the camera"))
		prompt_label.set_markup(f"<big><span foreground='#2AAAE5'><b>{N_(GUIDE_STEPS[0][1])}</b></span></big>")

		# Give the user time to read
		deadline = clock() + 2
		while clock() < deadline:
			get_frame()

		# Loop through frames till we hit a timeout
		for _ in range(60):
			# Get all faces from that frame as encodings
			frame, face_locations = get_frame()

			# If we've found at least one, we can continue
			if face_locations:
				break

		# If we've found no faces, try to determine why
		if not face_locations:
			video_capture.release()
			if valid_frames == 0:
				reason = N_("Camera saw only black frames - is IR emitter working?")
			elif valid_frames == dark_tries:
				reason = N_("All frames were too dark, please check dark_threshold in config") + " " + \
					N_("Average darkness: {avg}, Threshold: {threshold}").format(avg=str(dark_running_total / valid_frames), threshold=str(dark_threshold))
			else:
				reason = N_("No face detected, aborting")
			print(reason)
			sys.exit(1)

		# If more than 1 faces are detected we can't know which one belongs to the user
		elif len(face_locations) > 1:
			video_capture.release()
			print(N_("Multiple faces detected, aborting"))
			sys.exit(1)

		first_sample = encode(frame, face_locations[0])
		samples = [list(first_sample)]
		total = min(max_samples, 1 + len(steps) * samples_per_step)

		try:
			events.put(("progress", len(samples), total))

			for key, prompt in steps:
				if cancel.is_set() or len(samples) >= max_samples:
					break

				events.put(("emit", guide_line(key)))
				events.put(("emit", prompt))

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

		if finished.is_set():
			loop.quit()
			return glib.SOURCE_REMOVE

		return glib.SOURCE_CONTINUE

	dialog.show_all()

	loop = glib.MainLoop()
	glib.timeout_add(33, update_ui)

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

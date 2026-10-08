# Compare incoming video with known faces
# Running in a local python instance to get around PATH issues

import signal

# Import time so we can start timing asap
import time

# Start timing
timings = {
	"st": time.time()
}

# Import required modules
import os
import sys
import json
import configparser
import dlib
import cv2
import re
from datetime import timezone, datetime
import atexit
import subprocess
import snapshot
import numpy as np
import _thread as thread
import paths_factory
from recorders.video_capture import VideoCapture
from i18n import _
from exit_codes import CompareResult

# Force singlethreaded BLAS. dlib's OpenBLAS/OpenMP matmuls can livelock when raced against this scri>
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("GOTO_NUM_THREADS", "1")

video_capture = None
_cleaned_up = False

# shamelessly copied from psutil
def process_exists(pid):
    """Check whether pid exists in the current process table."""
    if pid == 0:
        # According to "man 2 kill" PID 0 has a special meaning:
        # it refers to <<every process in the process group of the
        # calling process>> so we don't want to go any further.
        # If we get here it means this UNIX platform *does* have
        # a process with id 0.
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # EPERM clearly means there's a process to deny access to
        return True
    # According to "man 2 kill" possible error values are
    # (EINVAL, EPERM, ESRCH)
    else:
        return True

def install_parent_death_signal(sig=signal.SIGTERM):
	"""Ask the kernel to signal us if the PAM parent exits (Esc/cancel).

	Prevents an orphaned compare from holding the camera on the lock shield
	when gdm-session-worker dies without a clean PAM return.
	"""
	try:
		import ctypes

		libc = ctypes.CDLL("libc.so.6", use_errno=True)
		# PR_SET_PDEATHSIG == 1 on Linux
		return libc.prctl(1, sig) == 0
	except Exception:
		return False

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

def _session_idle_hint():
	"""Return True/False for the current login1 session's IdleHint, or None
	if it can't be determined (busctl missing, no session, DBus error, ...).
	"""
	try:
		session_path = subprocess.run(
			[BUSCTL_PATH, "--system", "--", "call", "org.freedesktop.login1", "/org/freedesktop/login1",
			 "org.freedesktop.login1.Manager", "GetSessionByPID", "u", str(os.getpid())],
			capture_output=True, text=True, timeout=2,
		)
		if session_path.returncode != 0:
			return None
		# Output looks like: o "/org/freedesktop/login1/session/_31"
		parts = session_path.stdout.strip().split(None, 1)
		if len(parts) != 2 or parts[0] != "o":
			return None
		path = parts[1].strip().strip('"')

		idle_hint = subprocess.run(
			[BUSCTL_PATH, "--system", "--", "get-property", "org.freedesktop.login1", path,
			 "org.freedesktop.login1.Session", "IdleHint"],
			capture_output=True, text=True, timeout=2,
		)
		if idle_hint.returncode != 0:
			return None
		# Output must be exactly "b true" or "b false" -- anything else
		# (malformed output, an unexpected D-Bus type signature like "s true")
		# is treated as undetermined rather than guessed, since a false
		# positive here would abort a legitimate scan.
		parts = idle_hint.stdout.strip().split()
		if len(parts) != 2 or parts[0] != "b" or parts[1] not in ("true", "false"):
			return None
		return parts[1] == "true"
	except Exception:
		return None

def _watch_session_idle(poll_interval=0.5):
	"""Abort the scan if our login1 session goes idle (Esc at a greeter/lock
	screen falling back to screensaver/idle). Runs on its own daemon thread;
	any failure here (busctl missing, DBus unavailable, parsing error) just
	means this watcher does nothing -- it must never block or interfere with
	the normal auth flow.

	Edge-triggered, not level-triggered: waking a lock screen commonly starts
	a new auth attempt *while* IdleHint is still true from before (it's only
	cleared on successful unlock), so aborting on the first poll would reject
	the normal case. This only aborts on an observed False -> True transition
	during this attempt, requiring at least one "not idle" reading first.

	Also does not touch the camera/GTK subprocess directly: cleanup() runs on
	the main thread, which may concurrently be reading from video_capture or
	writing to gtk_proc's stdin. Instead this sends the process SIGTERM,
	which Python always delivers to the main thread's registered handler
	(_signal_exit), so cleanup happens exactly like an external cancel.
	"""
	consecutive_failures = 0
	seen_not_idle = False
	try:
		while not _cleaned_up:
			idle = _session_idle_hint()
			if idle is None:
				consecutive_failures += 1
				# Give up after a few consecutive failures rather than a
				# single one (avoid bailing out on a transient DBus hiccup),
				# but still stop eventually if this greeter/login manager or
				# platform genuinely doesn't expose session idle state.
				if consecutive_failures >= 3:
					return
				time.sleep(poll_interval)
				continue
			consecutive_failures = 0
			if idle:
				# Re-check _cleaned_up right before firing: _session_idle_hint()
				# can block for a couple of seconds (two busctl calls), long
				# enough for the main thread to have independently finished a
				# successful/failed exit via cleanup() in the meantime. Do not
				# signal a process that has already decided its own exit code
				# -- _signal_exit() is the authoritative backstop for the
				# remaining (much narrower) race, but avoiding the signal
				# entirely here is strictly better.
				if seen_not_idle and not _cleaned_up:
					os.kill(os.getpid(), signal.SIGTERM)
					return
				# Already idle when this attempt started -- not a
				# transition, so not our signal to abort. Keep watching for
				# an actual False -> True transition instead.
			else:
				seen_not_idle = True
			time.sleep(poll_interval)
	except Exception:
		pass

def cleanup():
	"""Release the camera and terminate the auth UI if still running"""
	global video_capture, gtk_proc, _cleaned_up

	if _cleaned_up:
		return
	_cleaned_up = True

	# Release the camera if we opened one (safe if missing/already released)
	if video_capture is not None:
		try:
			video_capture.release()
		except Exception:
			pass
		video_capture = None
		# Brief settle so the next PAM attempt can reopen the V4L device.
		try:
			time.sleep(0.35)
		except Exception:
			pass

	# Exit the auth ui process if there is one
	if "gtk_proc" in globals() and gtk_proc is not None:
		try:
			gtk_proc.terminate()
			try:
				gtk_proc.wait(timeout=1)
			except subprocess.TimeoutExpired:
				gtk_proc.kill()
				gtk_proc.wait(timeout=1)
		except Exception:
			pass

def exit(code=None):
	"""Exit while releasing the camera and closing howdy-gtk properly"""
	cleanup()

	# Exit compare
	if code is not None:
		if isinstance(code, CompareResult):
			sys.exit(code.value)
		sys.exit(code)


def _signal_exit(signum, frame):
	"""Handle SIGTERM/SIGINT: same cleanup as exit, then abort with code 12.

	If cleanup() has already run -- e.g. the main thread already completed
	its own successful/failed exit, or another signal got here first -- this
	is a no-op instead of forcing os._exit(12). Overriding an
	already-decided exit code (e.g. turning a successful 0 into an abort
	12) would be worse than not aborting. This is also the backstop against
	the idle watcher's SIGTERM racing a concurrent natural exit: the
	watcher's own _cleaned_up re-check narrows the window, but this check
	is what makes the remainder of that race harmless.
	"""
	if _cleaned_up:
		return
	cleanup()
	sys.exit(CompareResult.Error)

def init_detector(lock):
	"""Start face detector, encoder and predictor in a new thread"""
	global face_detector, pose_predictor, face_encoder

	# Test if at lest 1 of the data files is there and abort if it's not
	if not os.path.isfile(paths_factory.shape_predictor_5_face_landmarks_path()):
		print(_("Data files have not been downloaded, please run the following commands:"))
		print("\n\tcd " + paths_factory.dlib_data_dir_path())
		print("\tsudo ./install.sh\n")
		lock.release()
		exit(CompareResult.Error)

	# Use the CNN detector if enabled
	if use_cnn:
		face_detector = dlib.cnn_face_detection_model_v1(paths_factory.mmod_human_face_detector_path())
	else:
		face_detector = dlib.get_frontal_face_detector()

	# Start the others regardless
	pose_predictor = dlib.shape_predictor(paths_factory.shape_predictor_5_face_landmarks_path())
	face_encoder = dlib.face_recognition_model_v1(paths_factory.dlib_face_recognition_resnet_model_v1_path())

	# Note the time it took to initialize detectors
	timings["ll"] = time.time() - timings["ll"]
	lock.release()


def make_snapshot(type):
	"""Generate snapshot after detection"""
	snapshot.generate(snapframes, [
		type + _(" LOGIN"),
		_("Date: ") + datetime.now(timezone.utc).strftime("%Y/%m/%d %H:%M:%S UTC"),
		_("Scan time: ") + str(round(time.time() - timings["fr"], 2)) + "s",
		_("Frames: ") + str(frames) + " (" + str(round(frames / (time.time() - timings["fr"]), 2)) + "FPS)",
		_("Hostname: ") + os.uname().nodename,
		_("Best certainty value: ") + str(round(lowest_certainty * 10, 1))
	])


def send_to_ui(type, message):
	"""Send message to the auth ui"""
	global gtk_proc

	# Only execute of the process started
	if "gtk_proc" in globals():
		# Format message so the ui can parse it
		message = type + "=" + message + " \n"

		# Try to send the message to the auth ui, but it's okay if that fails
		try:
			if gtk_proc.poll() is None: # Make sure the gtk_proc is still running before write into the pipe
				gtk_proc.stdin.write(bytearray(message.encode("utf-8")))
				gtk_proc.stdin.flush()
		except IOError:
			pass


install_parent_death_signal()

# Make sure we were given an username to test against and a parent pid
if len(sys.argv) < 3:
	exit(CompareResult.Abort)

# The username of the user being authenticated
user = sys.argv[1]
# The pid of the process calling us
parent_pid = int(sys.argv[2])
# The model file contents
models = []
# Encoded face models
encodings = []
# Amount of ignored 100% black frames
black_tries = 0
# Amount of ignored dark frames
dark_tries = 0
# Total amount of frames captured
frames = 0
# Captured frames for snapshot capture
snapframes = []
# Tracks the lowest certainty value in the loop
lowest_certainty = 10
# Frames whose best match already fell inside the threshold. One frame under
# the bar is a weak signal: a scan produces dozens of independent looks, so
# the frame that decides authentication is the *minimum* of many draws, and a
# single blurred or oddly lit outlier can carry a stranger through. Demanding
# several agreeing frames removes that outlier without moving the threshold.
confirmations_seen = 0
# Face recognition/detection instances
face_detector = None
pose_predictor = None
face_encoder = None

# Try to load the face model from the models folder
try:
	models = json.load(open(paths_factory.user_model_path(user)))

	for model in models:
		encodings += model["data"]
except FileNotFoundError:
	exit(CompareResult.NoFaceModel)

# Check if the file contains a model
if len(models) < 1:
	exit(CompareResult.NoFaceModel)

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

# Get all config values needed
use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)
timeout = str_to_seconds(config.get("face_authentication", "timeout", fallback="4s"))
dark_threshold = config.getfloat("video", "dark_threshold", fallback=50.0)
video_certainty = config.getfloat("video", "certainty", fallback=3.5) / 10
# Separate frames that must agree before a match is trusted. 1 is the
# historical single-frame behaviour and stays the fallback for configs
# written before this key existed; the Secure level ships 3.
video_confirmations = max(1, config.getint("video", "confirmations", fallback=1))
end_report = config.getboolean("debug", "end_report", fallback=False)
save_failed = config.getboolean("snapshots", "save_failed", fallback=False)
save_successful = config.getboolean("snapshots", "save_successful", fallback=False)
gtk_stdout = config.getboolean("debug", "gtk_stdout", fallback=False)
rotate = config.getint("video", "rotate", fallback=0)

if len(sys.argv) > 3:
	timeout = str_to_seconds(sys.argv[3])

if timeout < 1:
	timeout = sys.maxsize;

# Send the gtk output to the terminal if enabled in the config
gtk_pipe = sys.stdout if gtk_stdout else subprocess.DEVNULL

# Start the auth ui, register it to be always be closed on exit
try:
	gtk_proc = subprocess.Popen(["howdy-gtk", "--start-auth-ui"], stdin=subprocess.PIPE, stdout=gtk_pipe, stderr=gtk_pipe)
	atexit.register(exit)
except FileNotFoundError:
	pass

signal.signal(signal.SIGTERM, _signal_exit)
signal.signal(signal.SIGINT, _signal_exit)

# Abort if the greeter/lock session goes idle mid-scan (e.g. Esc falling
# back to screensaver) -- see AGENTS.md lifecycle notes: this watches our
# own session's state and decides to bail out on its own, it does not
# touch PAM-level password watching or the workaround= hard rule.
if config.getboolean("core", "abort_on_session_idle", fallback=True):
	import threading
	threading.Thread(target=_watch_session_idle, daemon=True).start()

# Write to the stdin to redraw ui
send_to_ui("M", _("Starting up..."))

# Save the time needed to start the script
timings["in"] = time.time() - timings["st"]

# Import face recognition, takes some time
timings["ll"] = time.time()

# Start threading and wait for init to finish
lock = thread.allocate_lock()
lock.acquire()
thread.start_new_thread(init_detector, (lock, ))

# Start video capture on the IR camera
timings["ic"] = time.time()

video_capture = VideoCapture(config)

# Read exposure from config to use in the main loop
exposure = config.getint("video", "exposure", fallback=-1)

# Note the time it took to open the camera
timings["ic"] = time.time() - timings["ic"]

# wait for thread to finish
lock.acquire()
lock.release()
del lock

# Fetch the max frame height
max_height = config.getfloat("video", "max_height", fallback=320.0)

# Get the height of the image (which would be the width if screen is portrait oriented)
height = video_capture.internal.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1
if rotate == 2:
	height = video_capture.internal.get(cv2.CAP_PROP_FRAME_WIDTH) or 1
# Calculate the amount the image has to shrink
scaling_factor = (max_height / height) or 1

# Initiate histogram equalization
clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

# Let the ui know that we're ready
send_to_ui("M", _("Identifying you..."))

# Start the read loop
frames = 0
valid_frames = 0
timings["fr"] = time.time()
dark_running_total = 0

while process_exists(parent_pid):
	# Increment the frame count every loop
	frames += 1

	# Form a string to let the user know we're real busy
	ui_subtext = "Scanned " + str(valid_frames - dark_tries) + " frames"
	if (dark_tries > 1):
		ui_subtext += " (skipped " + str(dark_tries) + " dark frames)"
	# Show it in the ui as subtext
	send_to_ui("S", ui_subtext)

	# Stop if we've exceeded the time limit
	if time.time() - timings["fr"] > timeout:
		# Create a timeout snapshot if enabled
		if save_failed:
			make_snapshot(_("FAILED"))

		if dark_tries == valid_frames:
			print(_("All frames were too dark, please check dark_threshold in config"))
			print(_("Average darkness: {avg}, Threshold: {threshold}").format(avg=str(dark_running_total / max(1, valid_frames)), threshold=str(dark_threshold)))
			exit(CompareResult.TooDark)
		else:
			exit(CompareResult.TimeoutReached)

	# Grab a single frame of video
	frame, gsframe = video_capture.read_frame()
	gsframe = clahe.apply(gsframe)

	# If snapshots have been turned on
	if save_failed or save_successful:
		# Start capturing frames for the snapshot
		if len(snapframes) < 3:
			snapframes.append(frame)

	# Create a histogram of the image with 8 values
	hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256])
	# All values combined for percentage calculation
	hist_total = np.sum(hist)

	# Calculate frame darkness
	darkness = (hist[0] / hist_total * 100)

	# If the image is fully black due to a bad camera read,
	# skip to the next frame
	if (hist_total == 0) or (darkness == 100):
		black_tries += 1
		continue

	dark_running_total += darkness
	valid_frames += 1

	# If the image exceeds darkness threshold due to subject distance,
	# skip to the next frame
	if (darkness > dark_threshold):
		dark_tries += 1
		continue

	# If the height is too high
	if scaling_factor != 1:
		# Apply that factor to the frame
		frame = cv2.resize(frame, None, fx=scaling_factor, fy=scaling_factor, interpolation=cv2.INTER_AREA)
		gsframe = cv2.resize(gsframe, None, fx=scaling_factor, fy=scaling_factor, interpolation=cv2.INTER_AREA)

	# If camera is configured to rotate = 1, check portrait in addition to landscape
	if rotate == 1:
		if frames % 3 == 1:
			frame = cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
			gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_COUNTERCLOCKWISE)
		if frames % 3 == 2:
			frame = cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE)
			gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_CLOCKWISE)

	# If camera is configured to rotate = 2, check portrait orientation
	elif rotate == 2:
		if frames % 2 == 0:
			frame = cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
			gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_COUNTERCLOCKWISE)
		else:
			frame = cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE)
			gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_CLOCKWISE)

	# Get all faces from that frame as encodings
	# Upsamples 1 time
	face_locations = face_detector(gsframe, 1)
	# Loop through each face
	for fl in face_locations:
		if use_cnn:
			fl = fl.rect

		# Fetch the faces in the image
		face_landmark = pose_predictor(frame, fl)
		face_encoding = np.array(face_encoder.compute_face_descriptor(frame, face_landmark, 1))

		# Match this found face against a known face
		matches = np.linalg.norm(encodings - face_encoding, axis=1)

		# Get best match
		match_index = np.argmin(matches)
		match = matches[match_index]

		# Update certainty if we have a new low
		if lowest_certainty > match:
			lowest_certainty = match

		# Check if a match that's confident enough
		if 0 < match < video_certainty:
			confirmations_seen += 1
			if confirmations_seen < video_confirmations:
				# Keep scanning until enough separate frames agree. The break
				# leaves the per-face loop so one frame can only ever count
				# once, however many faces it happens to contain.
				break

			timings["tt"] = time.time() - timings["st"]
			timings["fl"] = time.time() - timings["fr"]

			# If set to true in the config, print debug text
			if end_report:
				def print_timing(label, k):
					"""Helper function to print a timing from the list"""
					print("  %s: %dms" % (label, round(timings[k] * 1000)))

				# Print a nice timing report
				print(_("Time spent"))
				print_timing(_("Starting up"), "in")
				print(_("  Open cam + load libs: %dms") % (round(max(timings["ll"], timings["ic"]) * 1000, )))
				print_timing(_("  Opening the camera"), "ic")
				print_timing(_("  Importing recognition libs"), "ll")
				print_timing(_("Searching for known face"), "fl")
				print_timing(_("Total time"), "tt")

				print(_("\nResolution"))
				width = video_capture.fw or 1
				print(_("  Native: %dx%d") % (height, width))
				# Save the new size for diagnostics
				scale_height, scale_width = frame.shape[:2]
				print(_("  Used: %dx%d") % (scale_height, scale_width))

				# Show the total number of frames and calculate the FPS by dividing it by the total scan time
				print(_("\nFrames searched: %d (%.2f fps)") % (frames, frames / timings["fl"]))
				print(_("Black frames ignored: %d ") % (black_tries, ))
				print(_("Dark frames ignored: %d ") % (dark_tries, ))
				print(_("Certainty of winning frame: %.3f") % (match * 10, ))

				print(_("Winning model: %d (\"%s\")") % (match_index, models[match_index]["label"]))

			# Make snapshot if enabled
			if save_successful:
				make_snapshot(_("SUCCESSFUL"))

			# Run rubberstamps if enabled
			if config.getboolean("rubberstamps", "enabled", fallback=False):
				import rubberstamps

				send_to_ui("S", "")

				if "gtk_proc" not in vars():
					gtk_proc = None

				rubberstamps.execute(config, gtk_proc, {
					"video_capture": video_capture,
					"face_detector": face_detector,
					"pose_predictor": pose_predictor,
					"clahe": clahe
				})

			# End peacefully
			exit(CompareResult.Success)

	if exposure != -1:
		# For a strange reason on some cameras (e.g. Lenoxo X1E) setting manual exposure works only after a couple frames
		# are captured and even after a delay it does not always work. Setting exposure at every frame is reliable though.
		video_capture.internal.set(cv2.CAP_PROP_AUTO_EXPOSURE, 1.0)  # 1 = Manual
		video_capture.internal.set(cv2.CAP_PROP_EXPOSURE, float(exposure))

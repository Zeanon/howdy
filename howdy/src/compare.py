# Guard against multiprocessing.forkserver re-importing this module.
# OpenVINO GPU runtime spawns worker processes via forkserver, which
# re-executes the main module. Without this guard, workers re-run the
# camera setup, hit EBUSY on /dev/video*, and break the parent process
# with "Failed to read camera" errors and PAM timeouts.
import sys as _sys
if __name__ != "__main__":
	_sys.exit(0)

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
import syslog
import json
import configparser
import dlib
import cv2
from datetime import timezone, datetime
import atexit
import pwd
import subprocess
import snapshot
import numpy as np
import _thread as thread
import paths_factory
import traceback

from enroll_capture import flatten_models
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

def cleanup():
	"""Release the camera and terminate the auth UI if still running"""
	global video_capture, gtk_proc, _cleaned_up

	if _cleaned_up:
		return
	_cleaned_up = True


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

	# Release the camera if we opened one (safe if missing/already released)
	if video_capture is not None:
		try:
			video_capture.release()
		except Exception:
			pass
		video_capture = None
		# Brief settle so the next PAM attempt can reopen the V4L device.
		# try:
		# 	time.sleep(0.35)
		# except Exception:
		# 	pass

def exit(code=None):
	"""Exit while releasing the camera and closing howdy-gtk properly"""
	cleanup()

	# Exit compare
	if code is not None:
		if isinstance(code, CompareResult):
			sys.exit(code.value)
		sys.exit(code)


def _signal_exit(signum, frame):
	if _cleaned_up:
		return
	cleanup()
	sys.exit(CompareResult.Error)

def init_detector(lock):
	"""Start face detector, encoder and predictor in a new thread"""
	global face_detector, face_encoder, pose_predictor, use_openvino

	# Test if at lest 1 of the data files is there and abort if it's not
	if not os.path.isfile(paths_factory.shape_predictor_5_face_landmarks_path()):
		print(_("Data files have not been downloaded, please run the following commands:"))
		print("\n\tcd " + paths_factory.dlib_data_dir_path())
		print("\tsudo ./install.sh\n")
		lock.release()
		exit(CompareResult.Error)

	if use_openvino:
		try:
			import openvino_face
			if openvino_face.is_available():
				face_detector = openvino_face.FaceDetector("GPU")
				face_encoder = openvino_face.FaceEncoder("GPU")
		except Exception as e:
			use_openvino = False
			print(f"OpenVINO not available ({e}), using dlib")
			syslog.syslog(
				syslog.LOG_ERR,
				f"OpenVINO init failed: {e!r}"
			)
			syslog.syslog(
				syslog.LOG_ERR,
				traceback.format_exc()
			)
	if not use_openvino:
		if use_cnn:
			face_detector = dlib.cnn_face_detection_model_v1(paths_factory.mmod_human_face_detector_path())
		else:
			face_detector = dlib.get_frontal_face_detector()
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

def get_graphical_session_env():
    sessions = subprocess.run(
        ["loginctl", "list-sessions", "--no-legend", "--no-pager"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()

    candidates = []

    for line in sessions:
        fields = line.split()
        if not fields:
            continue

        session_id = fields[0]

        result = subprocess.run(
            [
                "loginctl", "show-session", session_id,
                "--property=Name",
                "--property=User",
                "--property=Type",
                "--property=Class",
                "--property=Active",
                "--property=Remote",
                "--property=State",
            ],
            capture_output=True,
            text=True,
            check=True,
        )

        props = dict(
            entry.split("=", 1)
            for entry in result.stdout.splitlines()
            if "=" in entry
        )

        if (
            props.get("Active") == "yes"
            and props.get("Remote") == "no"
            and props.get("Class") == "user"
            and props.get("Type") in ("wayland", "x11")
        ):
            candidates.append((session_id, props))

    if not candidates:
        raise RuntimeError("Keine aktive lokale grafische Sitzung gefunden")

    # Bevorzuge den Prozess der Plasma-Sitzung, um deren tatsächliche
    # Display-Variablen statt angenommener Standardwerte zu verwenden.
    for session_id, props in candidates:
        uid = int(props["User"])
        runtime_dir = f"/run/user/{uid}"

        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue

            try:
                if os.stat(f"/proc/{pid}").st_uid != uid:
                    continue

                with open(f"/proc/{pid}/comm") as f:
                    comm = f.read().strip()

                if comm not in ("plasmashell", "kwin_wayland", "kwin_x11"):
                    continue

                with open(f"/proc/{pid}/environ", "rb") as f:
                    entries = f.read().split(b"\0")

                proc_env = {}
                for entry in entries:
                    if b"=" in entry:
                        key, value = entry.split(b"=", 1)
                        proc_env[key.decode()] = value.decode(
                            errors="replace"
                        )

                # Nur Prozesse aus der passenden grafischen Sitzung.
                if proc_env.get("XDG_SESSION_ID") not in (
                    None, session_id
                ):
                    continue

                env = {
                    "HOME": pwd.getpwuid(uid).pw_dir,
                    "USER": pwd.getpwuid(uid).pw_name,
                    "LOGNAME": pwd.getpwuid(uid).pw_name,
                    "XDG_RUNTIME_DIR": runtime_dir,
                    "DBUS_SESSION_BUS_ADDRESS":
                        f"unix:path={runtime_dir}/bus",
                }

                for key in (
                    "DISPLAY",
                    "WAYLAND_DISPLAY",
                    "XAUTHORITY",
                ):
                    if proc_env.get(key):
                        env[key] = proc_env[key]

                if (
                    env.get("WAYLAND_DISPLAY")
                    or env.get("DISPLAY")
                ):
                    return env

            except (OSError, ValueError, KeyError):
                continue

    raise RuntimeError(
        "Display-Umgebung der aktiven Plasma-Sitzung nicht gefunden"
    )



install_parent_death_signal()

# Make sure we were given an username to test against and a parent pid
if len(sys.argv) < 4:
	exit(CompareResult.Abort)

# The username of the user being authenticated
user = sys.argv[1]
# The pid of the process calling us
parent_pid = int(sys.argv[2])
# Whether we should show the auth popup
auth_popup = int(sys.argv[3])
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
use_openvino = False

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

# Get all config values needed
use_cnn = config.getboolean("face_authentication", "use_cnn", fallback=False)
use_openvino = config.getboolean("face_authentication", "use_openvino", fallback=False)
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

if len(sys.argv) > 4:
	timeout = str_to_seconds(sys.argv[4])

if timeout < 1:
	timeout = sys.maxsize;

# Send the gtk output to the terminal if enabled in the config
gtk_pipe = sys.stdout if gtk_stdout else subprocess.DEVNULL

if auth_popup:
	# Start the auth ui, register it to be always be closed on exit
	try:
		gtk_env = os.environ.copy()
		gtk_env.update(get_graphical_session_env())
		gtk_proc = subprocess.Popen(["/usr/local/bin/howdy-gtk", "--start-auth-ui"], stdin=subprocess.PIPE, stdout=gtk_pipe, stderr=gtk_pipe, env=gtk_env)
		atexit.register(exit)
	except FileNotFoundError:
		pass

signal.signal(signal.SIGTERM, _signal_exit)
signal.signal(signal.SIGINT, _signal_exit)

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

# Try to load the face model from the models folder
try:
	models = json.load(open(paths_factory.user_model_path(user)))
	encoding_size = 256 if use_openvino else 128
	encodings, model_owner = flatten_models(models, encoding_size)
except FileNotFoundError:
	exit(CompareResult.NoFaceModel)

# Check if the file contains a model
if len(encodings) < 1:
	exit(CompareResult.NoFaceModel)

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
	if dark_tries > 1:
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
	if darkness > dark_threshold:
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
		if use_cnn and not use_openvino:
			fl = fl.rect

		# Fetch the faces in the image
		if use_openvino:
			face_encoding = face_encoder.encode(frame, fl)
			if face_encoding is None:
				continue
			matches = 1.0 - np.dot(np.array(encodings), face_encoding)
		else:
			face_landmark = pose_predictor(frame, fl)
			face_encoding = np.array(face_encoder.compute_face_descriptor(frame, face_landmark, 1))
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

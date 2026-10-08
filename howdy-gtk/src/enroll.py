# Run `howdy add` from the GTK apps with live on-screen guidance.
#
# `add` walks the user through a few small head movements and reports them
# on stdout as machine-readable lines (see howdy/src/enroll_capture.py):
#
#     @guide <key>          the pose the user should take now
#     @progress <n>/<total> samples captured so far
#
# The command is run on a worker thread reading stdout line by line; every
# update is handed back to the caller on the GTK main loop, so the wizard and
# the Settings dialog can show "Turn your head slightly to the left" and
# "Recording… 3 of 13" while the camera is busy. `subprocess.run` with
# capture_output used to block the main loop for the whole scan.

import subprocess
import threading

from gi.repository import GLib

from i18n import _

# Same keys as enroll_capture.GUIDE_STEPS; translated in the GTK domain.
GUIDE_TEXT = {
	"center": lambda: _("Look straight at the camera"),
	"left": lambda: _("Turn your head slightly to the left"),
	"right": lambda: _("Turn your head slightly to the right"),
	"up": lambda: _("Tilt your chin up a little"),
	"down": lambda: _("Tilt your chin down a little"),
}


def guide_text(key):
	"""User-facing prompt for a `@guide` key ('' for unknown keys)."""
	fn = GUIDE_TEXT.get(key)
	return fn() if fn else ""


def progress_text(count, total):
	return _("Recording… {count} of {total}").format(count=count, total=total)


def parse_line(line):
	"""('guide', key) | ('progress', (n, total)) | None for plain output."""
	line = line.strip()
	if line.startswith("@guide "):
		key = line[len("@guide "):].strip()
		return ("guide", key) if key else None
	if line.startswith("@progress "):
		try:
			count, total = line[len("@progress "):].strip().split("/", 1)
			return ("progress", (int(count), int(total)))
		except ValueError:
			return None
	return None


def is_protocol_line(line):
	return line.lstrip().startswith("@")


def _pump_output(proc, on_guide, on_progress, dispatch, lines):
	"""Forward protocol lines to the callbacks, collecting everything else.

	Returns the process exit status.
	"""
	try:
		for line in proc.stdout:
			parsed = parse_line(line)
			if parsed is None:
				if not is_protocol_line(line):
					lines.append(line)
				continue
			kind, value = parsed
			if kind == "guide":
				dispatch(on_guide, value)
			else:
				dispatch(on_progress, value[0], value[1])
		return proc.wait()
	except Exception as exc:
		lines.append(str(exc))
		return getattr(proc, "returncode", None) or 1


def run_add(cmd, on_guide, on_progress, on_done, popen=None, dispatch=None):
	"""Start `cmd` on a worker thread; callbacks run on the GTK main loop.

	on_guide(key)              on_progress(count, total)
	on_done(status, output)    output = everything printed, protocol lines removed
	Returns the thread. `popen` / `dispatch` are injectable for tests.
	"""
	popen = popen or subprocess.Popen
	dispatch = dispatch or GLib.idle_add

	def worker():
		try:
			proc = popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
		except Exception as exc:  # FileNotFoundError, PermissionError…
			dispatch(on_done, 127, str(exc))
			return

		lines = []
		status = _pump_output(proc, on_guide, on_progress, dispatch, lines)
		dispatch(on_done, status, "".join(lines))

	thread = threading.Thread(target=worker, daemon=True)
	thread.start()
	return thread

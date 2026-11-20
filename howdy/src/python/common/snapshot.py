# Create and save snapshots of auth attempts

# Import modules
import os
import time
import syslog
import cv2

import numpy as np

from datetime import datetime, UTC

from . import paths_factory


def generate(frames, text_lines):
	"""Generate a snapshot from given frames"""

	# Don't execute if no frames were given
	if len(frames) == 0:
		return

	# Get frame dimensions
	frame_height, frame_width, cc = frames[0].shape
	# Spread the given frames out horizontally
	snap = np.concatenate(frames, axis=1)

	# Create colors
	pad_color = [44, 44, 44]
	text_color = [255, 255, 255]

	# Add a gray square at the bottom of the image
	snap = cv2.copyMakeBorder(
		snap, 0, len(text_lines) * 20 + 40, 0, 0, cv2.BORDER_CONSTANT, value=pad_color
	)

	# Add the Howdy logo if there's space to do so
	if len(frames) > 1:
		# Load the logo from file
		logo = cv2.imread(paths_factory.logo_path())
		# Calculate the position of the logo
		logo_y = frame_height + 20
		logo_x = frame_width * len(frames) - 210

		# Overlay the logo on top of the image
		snap[logo_y : logo_y + 57, logo_x : logo_x + 180] = logo

	# Go through each line
	line_number = 0
	for line in text_lines:
		# Calculate how far the line should be from the top
		padding_top = frame_height + 30 + (line_number * 20)
		# Print the line onto the image
		cv2.putText(
			snap,
			line,
			(30, padding_top),
			cv2.FONT_HERSHEY_SIMPLEX,
			0.4,
			text_color,
			0,
			cv2.LINE_AA,
		)

		line_number += 1

	# Create snapshot directory with owner-only permissions
	snapshots_dir = paths_factory.snapshots_dir_path()
	os.makedirs(snapshots_dir, mode=0o700, exist_ok=True)
	os.chmod(snapshots_dir, 0o700)

	# Generate a filename based on the current time
	filename = datetime.now(UTC).strftime("%Y%m%dT%H%M%S.jpg")
	filepath = paths_factory.snapshot_path(filename)

	# Create snapshot file with owner-only permissions
	fd = os.open(
		filepath,
		os.O_WRONLY | os.O_CREAT | os.O_EXCL,
		0o600,
	)

	try:
		with os.fdopen(fd, "wb") as file:
			success, encoded = cv2.imencode(".jpg", snap)

			if not success:
				raise RuntimeError("Failed to encode snapshot")

			file.write(encoded.tobytes())

	except Exception:
		if os.path.exists(filepath):
			os.unlink(filepath)
		raise

	# Return the saved file location
	return filepath


def prune(max_age, max_count):
	"""Delete the snapshots that are older than max_age seconds and, of the rest,
	the oldest ones beyond max_count. Zero means no limit. Only regular .jpg files
	of the snapshot directory are touched, links are not followed. Returns the
	number of deleted files."""
	if max_age <= 0 and max_count <= 0:
		return 0

	try:
		directory_fd = os.open(
			paths_factory.snapshots_dir_path(),
			os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
		)
	except FileNotFoundError:
		return 0

	try:
		files = []
		with os.scandir(directory_fd) as entries:
			for entry in entries:
				if entry.name.endswith(".jpg") and entry.is_file(follow_symlinks=False):
					files.append(
						(entry.stat(follow_symlinks=False).st_mtime, entry.name)
					)

		# Oldest first, so that everything that has to go is at the start
		files.sort()

		now = time.time()
		removed = 0
		for index, (modified, name) in enumerate(files):
			too_old = max_age > 0 and now - modified > max_age
			too_many = max_count > 0 and len(files) - index > max_count
			if not too_old and not too_many:
				break
			try:
				os.unlink(name, dir_fd=directory_fd)
				removed += 1
			except OSError as exc:
				syslog.syslog(
					syslog.LOG_WARNING,
					f"Could not delete the old snapshot {name}: {exc}",
				)
		return removed
	finally:
		os.close(directory_fd)

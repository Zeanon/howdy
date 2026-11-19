# Create a snapshot

# Import required modules
import sys
import configparser

from datetime import datetime, UTC

from ..i18n import S
from .. import snapshot
from .. import paths_factory
from ..exit_codes import CompareResult
from ..recorders.video_capture import VideoCapture, NoDeviceException

# Read the config
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

# Start video capture
try:
	video_capture = VideoCapture(config)
except NoDeviceException as exc:
	print(exc.message)
	sys.exit(CompareResult.InvalidDevice)

# Read a frame to activate emitters
try:
	video_capture.read_frame()
except NoDeviceException as exc:
	print(exc.message)
	sys.exit(CompareResult.InvalidDevice)

dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)

# COllection of recorded frames
frames = []

while True:
	# Grab a single frame of video
	frame, gsframe = video_capture.read_frame()

	# Add the frame to the list
	frames.append(frame)

	# Stop the loop if we have 4 frames
	if len(frames) >= 4:
		break

# Generate a snapshot image from the frames
file = snapshot.generate(
	frames,
	[
		S("GENERATED SNAPSHOT"),
		S("Date: ") + datetime.now(UTC).strftime("%Y/%m/%d %H:%M:%S UTC"),
		S("Dark threshold config: ")
		+ str(config.getfloat("video", "dark_threshold", fallback=60.0)),
		S("Certainty config: ")
		+ str(config.getfloat("video", "certainty", fallback=3.5)),
	],
)

# Show the file location in console
print(S("Generated snapshot saved as"))
print(file)

# Open the config file in an editor

# Import required modules
import os
import sys
import errno
import shutil
import subprocess

from ..i18n import S
from .. import paths_factory


# Copied from the python elevate module (https://github.com/barneygale/elevate/blob/master/elevate/posix.py)
# Modified to accept environment variables so howdy-gtk starts properly
def elevate(env):
	if os.getuid() == 0:
		return

	env = ["env"] + env + ["sudo"]
	args = [sys.executable, "-I", "-m", "howdy.cli", *sys.argv[1:]]

	commands = [
		["pkexec"] + env + args,
		["gksudo"] + env + args,
		["kdesudo"] + env + args,
		["sudo"] + args,
	]

	for args in commands:
		try:
			os.execlp(args[0], *args)
		except OSError as exc:
			if exc.errno != errno.ENOENT or args[0] == "sudo":
				raise


# Make sure we run as sudo
elevate(
	env=[
		f"DISPLAY={os.environ.get('DISPLAY')}",
		f"XAUTHORITY={os.environ.get('XAUTHORITY')}",
		f"DBUS_SESSION_BUS_ADDRESS={os.environ.get('DBUS_SESSION_BUS_ADDRESS')}",
	]
)

# Check if we have rootish rights
# This is this far down the file so running the command for help is always possible
if os.geteuid() != 0:
	print(S("Please run this command as root:\n"))
	print("\tsudo howdy " + " ".join(sys.argv[1:]))
	sys.exit(1)

# Determine the editor to use
editor = None
preferred_editor = os.environ.get("EDITOR")
nano_path = shutil.which("nano")
vi_path = shutil.which("vi")

# Use the user preferred editor if available
if preferred_editor:
	if shutil.which(preferred_editor):
		editor = preferred_editor

if not editor:
	if nano_path:
		editor = nano_path
	elif vi_path:
		editor = vi_path

if editor:
	editor_name = os.path.basename(editor)

	# Let the user know what we're doing
	print(S("Opening config.ini in {}").format(editor_name))

	# Open the editor as a subprocess and fork it
	try:
		subprocess.call([editor, paths_factory.config_file_path()])
	except Exception as exc:
		print(S("Failed to open editor: {}").format(exc))

else:
	print(S("Error: Could not find a suitable text editor."))
	print(S("Please install 'nano' or 'vi', or set the EDITOR environment variable."))
	print(
		S(
			"If you are running this command with sudo, try 'sudo -E howdy config' to preserve your EDITOR variable."
		)
	)

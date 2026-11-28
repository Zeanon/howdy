# Open the config file in an editor

# Import required modules
import os
import sys
import errno
import shutil
import subprocess
import builtins

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


# Other graphical editors and the option that makes them stay until the file is closed
GRAPHICAL_EDITORS = {
	"code": "--wait",
	"codium": "--wait",
	"gedit": "--wait",
	"gnome-text-editor": "--standalone",
}

# They open the file as the user and ask for the password themselves to save it
SELF_SAVING_EDITORS = ("kate", "kwrite")


def edit_with(command):
	"""Edit the config in the editor that was named on the command line, with its
	options if there are any: "kate" or "vim -n". Ends the process."""
	program = command.split(" ")[0]
	if not shutil.which(program):
		print(S("Could not find the editor {}").format(program))
		sys.exit(1)
	name = os.path.basename(program)
	config_path = str(paths_factory.config_file_path())

	if os.geteuid() == 0:
		# Graphical editors refuse to run as root, or have no access to the screen of
		# the user
		if name in SELF_SAVING_EDITORS or name in GRAPHICAL_EDITORS:
			print(
				S(
					"{0} can not run as root, please run this command as your user:\n\n\thowdy config {0}"
				).format(name)
			)
			sys.exit(1)
		print(S("Opening config.ini in {}").format(name))
		sys.exit(1 if subprocess.call([*command.split(), config_path]) else 0)

	if name in SELF_SAVING_EDITORS:
		print(S("Opening config.ini in {}").format(name), flush=True)
		arguments = command.split()
		os.execvp(arguments[0], [*arguments, config_path])

	# sudoedit runs the editor as the user on a copy of the file and writes it back as
	# root
	if not shutil.which("sudoedit"):
		print(S("Choosing the editor needs sudoedit, which is not installed"))
		sys.exit(1)
	if name in GRAPHICAL_EDITORS and command == program:
		command += " " + GRAPHICAL_EDITORS[name]
	os.environ["SUDO_EDITOR"] = command
	print(S("Opening config.ini in {}").format(name), flush=True)
	os.execlp("sudoedit", "sudoedit", "--", config_path)


# Before the elevation: the named editor is run as the user
if builtins.howdy_args.arguments:
	edit_with(builtins.howdy_args.arguments[0])

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

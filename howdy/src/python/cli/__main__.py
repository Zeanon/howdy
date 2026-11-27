# Guard against multiprocessing.forkserver re-importing this module.
# OpenVINO GPU runtime spawns worker processes via forkserver, which
# re-executes the main module (cli.py when running any `howdy <cmd>`).
# Without this guard, workers re-run the CLI dispatch and break the
# parent process with camera EBUSY errors and argparse side effects.
import sys

if __name__ != "__main__":
	sys.exit(0)

# CLI directly called by running the howdy command

# Import required modules
import os
import pwd
import signal
import getpass
import argparse
import builtins

from ..i18n import S  # type: ignore

# Try to get the original username (not "root") from shell
# sudo_user = os.environ.get("SUDO_USER")
# doas_user = os.environ.get("DOAS_USER")
# pkexec_uid = os.environ.get("PKEXEC_UID")
# pkexec_user = pwd.getpwuid(int(pkexec_uid))[0] if pkexec_uid else ""
# env_user = getpass.getuser()
# user = next((u for u in [sudo_user, doas_user, pkexec_user, env_user] if u), "")
user = getpass.getuser()

# If that fails, error out
if user == "":
	print(S("Could not determine user, please use the --user flag"))
	sys.exit(1)

# Basic command setup
parser = argparse.ArgumentParser(
	description=S("Command line interface for Howdy face authentication."),
	formatter_class=argparse.RawDescriptionHelpFormatter,
	add_help=False,
	prog="howdy",
	usage=f"howdy [-U USER] [--plain] [-h] [-y] {S('command')} [{S('arguments')}...]",
	epilog=S("For support please visit\nhttps://github.com/boltgolt/howdy"),
)

# Add an argument for the command
parser.add_argument(
	"command",
	help=S(
		"The command option to execute, can be one of the following: add, clear, config, disable, list, remove, rename, snapshot, set, test or version."
	),
	metavar="command",
	choices=[
		"add",
		"clear",
		"config",
		"disable",
		"list",
		"remove",
		"rename",
		"set",
		"snapshot",
		"test",
		"version",
	],
)

# Add an argument for the extra arguments of disable and remove
parser.add_argument(
	"arguments",
	help=S("Optional arguments for the add, disable, remove, rename and set commands."),
	nargs="*",
)

# Add the user flag
parser.add_argument(
	"-U", "--user", default=None, help=S("Set the user account to use.")
)

# Add the -y flag
parser.add_argument("-y", help=S("Skip all questions."), action="store_true")

# Add the --plain flag
parser.add_argument(
	"--plain", help=S("Print machine-friendly output."), action="store_true"
)

# Overwrite the default help message so we can use a uppercase S
parser.add_argument(
	"-h",
	"--help",
	action="help",
	default=argparse.SUPPRESS,
	help=S("Show this help message and exit."),
)

parser.add_argument("--gui", action="store_true")

# If we only have 1 argument we print the help text
if len(sys.argv) < 2:
	print(S("current active user: ") + user + "\n")
	parser.print_help()
	sys.exit(0)

# Parse all arguments above
args = parser.parse_args()

# Save the args and user as builtins which can be accessed by the imports
builtins.howdy_args = args

root_commands = ["config", "set", "disable"]

if args.user is None:
	# The test command runs as root through pkexec or sudo, but is meant for the user
	# that started it. So are the other commands for a user, like sudo howdy add
	if args.command == "test" or (user == "root" and args.command not in root_commands):
		sudo_user = os.environ.get("SUDO_USER")
		doas_user = os.environ.get("DOAS_USER")
		pkexec_uid = os.environ.get("PKEXEC_UID")
		pkexec_user = pwd.getpwuid(int(pkexec_uid))[0] if pkexec_uid else ""
		user = next((u for u in [sudo_user, doas_user, pkexec_user, user] if u), "")
	args.user = user

# Beyond this point the user can't change anymore, if we still have root as user we need to abort
if args.user == "root" and args.command not in root_commands:
	print(
		S(
			"Can't run howdy commands as root, please run this command with the --user flag"
		)
	)
	sys.exit(1)

signal.signal(signal.SIGINT, signal.SIG_DFL)

# fmt: off
# Execute the right command. The modules without run() do it when they are imported
if args.command == "add":
	from . import add
	add.run()
elif args.command == "clear":
	from . import clear
	clear.run()
elif args.command == "config":
	from . import config  # noqa: F401  (runs its command when it is imported)
elif args.command == "disable":
	from . import disable  # noqa: F401  (runs its command when it is imported)
elif args.command == "list":
	from . import list
	list.run()
elif args.command == "remove":
	from . import remove
	remove.run()
elif args.command == "rename":
	from . import rename
	rename.run()
elif args.command == "set":
	from . import set  # noqa: F401  (runs its command when it is imported)
elif args.command == "snapshot":
	from . import snap  # noqa: F401  (runs its command when it is imported)
elif args.command == "test":
	from . import test  # noqa: F401  (runs its command when it is imported)
else:
	print("Howdy 3.0.1")
# fmt: on

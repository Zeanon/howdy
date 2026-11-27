# Set the disable flag

# Import required modules
import os
import sys
import builtins
import fileinput
import configparser

from ..i18n import S
from .. import paths_factory

# Check if we have rootish rights
# This is this far down the file so running the command for help is always possible
if os.geteuid() != 0:
	print(S("Please run this command as root:\n"))
	print("\tsudo howdy " + " ".join(sys.argv[1:]))
	sys.exit(1)

# Get the absolute filepath
config_path = paths_factory.config_file_path()

# Read config from disk
config = configparser.ConfigParser()
config.read(config_path)

# Check if enough arguments have been passed
if not builtins.howdy_args.arguments:
	print(S("Please add a 0 (enable) or a 1 (disable) as an argument"))
	sys.exit(1)

# Get the cli argument
argument = builtins.howdy_args.arguments[0]

# Translate the argument to the right string
if argument == "1" or argument.lower() == "true":
	out_value = "true"
elif argument == "0" or argument.lower() == "false":
	out_value = "false"
else:
	# Of it's not a 0 or a 1, it's invalid
	print(S("Please only use 0 (enable) or 1 (disable) as an argument"))
	sys.exit(1)

current = config.get("core", "disabled", fallback="")
if not current:
	print('Could not find the "disabled" option in ' + str(config_path))
	sys.exit(1)

# Don't do anything when the state is already the requested one
if out_value == current:
	print(S("The disable option has already been set to ") + out_value)
	sys.exit(1)

# Only the line of the disable config is replaced, written like that
search = "disabled = " + current
with open(config_path) as config_file:
	if search not in config_file.read():
		# Nothing would change, Howdy must not be said to be disabled then
		print(f'Could not find "{search}" in {config_path}')
		sys.exit(1)

# Loop though the config file and only replace the line containing the disable config
for line in fileinput.input([config_path], inplace=1):
	print(line.replace(search, "disabled = " + out_value), end="")

# Print what we just did
if out_value == "true":
	print(S("Howdy has been disabled"))
else:
	print(S("Howdy has been enabled"))

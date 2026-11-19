# Set a config value

# Import required modules
import os
import sys
import builtins
import fileinput

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

# Check if enough arguments have been passed
if len(builtins.howdy_args.arguments) < 2:
	print(S("Please add a setting you would like to change and the value to set it to"))
	print(S("For example:"))
	print("\n\thowdy set certainty 3\n")
	sys.exit(1)

# Get the name and value from the cli
set_name = builtins.howdy_args.arguments[0]
set_value = builtins.howdy_args.arguments[1]

# Will be filled with the correctly config line to update
found_line = ""

# Loop through all lines in the config file
for line in fileinput.input([config_path]):
	# Save the line if it starts with the requested config option
	if line.startswith(set_name + " "):
		found_line = line

# If we don't have the line it is not in the config file
if not found_line:
	print(S(f'Could not find a "{set_name}" config option to set'))
	sys.exit(1)

# Go through the file again and update the correct line
for line in fileinput.input([config_path], inplace=1):
	print(line.replace(found_line, set_name + " = " + set_value + "\n"), end="")

print(S("Config option updated"))

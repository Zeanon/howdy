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

# "section.name" says which one is meant, several sections have an option of the same name
section, _, option = set_name.rpartition(".")


def option_lines():
	"""(number, section) of every line of the option in the wanted sections"""
	current = ""
	with open(config_path) as config_file:
		for number, line in enumerate(config_file):
			if line.startswith("["):
				current = line[1:].split("]")[0]
			elif line.startswith(option + " ") and section in ("", current):
				yield number, current


found = list(option_lines())

# If we don't have the line it is not in the config file
if not found:
	print(S(f'Could not find a "{set_name}" config option to set'))
	sys.exit(1)

sections = list(dict.fromkeys(name for _, name in found))
if len(sections) > 1:
	print(f'"{option}" is in several sections, say which one:')
	for name in sections:
		print(f"\thowdy set {name}.{option} {set_value}")
	sys.exit(1)

# Go through the file again and update the lines
numbers = {number for number, _ in found}
for number, line in enumerate(fileinput.input([config_path], inplace=1)):
	print(option + " = " + set_value + "\n" if number in numbers else line, end="")

print(S("Config option updated"))

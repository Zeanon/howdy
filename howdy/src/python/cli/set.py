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
	print(S('Could not find a "{}" config option to set').format(set_name))
	sys.exit(1)

sections = list(dict.fromkeys(name for _, name in found))
if len(sections) > 1:
	print(f'"{option}" is in several sections, say which one:')
	for name in sections:
		print(f"\thowdy set {name}.{option} {set_value}")
	sys.exit(1)


def continuation_lines(numbers):
	"""The indented lines behind an option belong to its value (stamp_rules has one
	rule on each), they go with the old value. Empty lines and comments between them
	stay."""
	inside = False
	with open(config_path) as config_file:
		for number, line in enumerate(config_file):
			text = line.strip()
			if number in numbers:
				inside = True
			elif not inside or not text or text.startswith(("#", ";")):
				continue
			elif line[0].isspace():
				yield number
			else:
				inside = False


# Go through the file again and update the lines
numbers = {number for number, _ in found}
removed = set(continuation_lines(numbers))
for number, line in enumerate(fileinput.input([config_path], inplace=1)):
	if number in removed:
		continue
	print(option + " = " + set_value + "\n" if number in numbers else line, end="")

print(S("Config option updated"))

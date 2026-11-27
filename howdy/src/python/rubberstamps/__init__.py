import os
import re
import time

from ..i18n import S

from importlib import import_module


class StampCancelled(Exception):
	"""Raised when the authentication was cancelled while a stamp was running"""


class RubberStamp:
	"""Howdy rubber stamp"""

	# threading.Event that is set when the authentication gets cancelled
	cancel_event = None
	# Called all the time a stamp runs, so that the popup stays
	keep_alive = None

	def check_cancelled(self):
		"""Stop the stamp if the authentication was cancelled, and keep the popup alive.
		Call this regularly."""
		if self.cancel_event is not None and self.cancel_event.is_set():
			raise StampCancelled()
		if self.keep_alive is not None:
			self.keep_alive()

	def sleep(self, seconds):
		"""Like time.sleep(), but wakes up early and stops the stamp on cancel"""
		end = time.monotonic() + seconds
		while True:
			self.check_cancelled()
			# In small steps, the popup is looked after
			left = min(end - time.monotonic(), 0.1)
			if left <= 0:
				return
			if self.cancel_event is not None:
				self.cancel_event.wait(left)
			else:
				time.sleep(left)

	def send_to_ui(self, message=None, subtext=None):
		"""Update the main text and/or the subtext of the auth popup"""
		if self.verbose:
			print(f"Sending to the auth popup: {message} | {subtext}")

		self._send_to_ui(message=message, subtext=subtext)


def execute(config, send_to_ui, opencv, cancel_event=None, keep_alive=None):
	"""Run all the stamps from the config, returns False if one of them denied the
	authentication.

	send_to_ui(message=None, subtext=None) updates the auth popup. opencv holds the
	video_capture, face_detector, pose_predictor and clahe of the authenticator.
	cancel_event is an optional threading.Event. If it gets set, the running stamp
	stops and StampCancelled is raised. keep_alive() is called while a stamp runs."""
	verbose = config.getboolean("debug", "verbose_stamps", fallback=False)
	dir_path = os.path.dirname(os.path.realpath(__file__))
	installed_stamps = []

	# Go through each file in the rubberstamp folder
	for filename in os.listdir(dir_path):
		# Remove non-readable file or directories
		if not os.path.isfile(dir_path + "/" + filename):
			continue

		# Remove meta files
		if filename in ["__init__.py", ".gitignore"]:
			continue

		# Add the found file to the list of enabled rubberstamps
		installed_stamps.append(filename.split(".")[0])

	if verbose:
		print("Installed rubberstamps: " + ", ".join(installed_stamps))

	# Get the rules defined in the config
	raw_rules = config.get("rubberstamps", "stamp_rules")
	rules = raw_rules.split("\n")

	# All rules are read before the first stamp runs: nobody is asked to nod for an
	# authentication that a later line denies anyway
	stamps = []

	for rule in rules:
		rule = rule.strip()

		if len(rule) <= 1:
			continue

		# Parse the rule with regex
		regex_result = re.search(
			r"^(\w+)\s+([\w\.]+)\s+([a-z]+)(.*)?$", rule, re.IGNORECASE
		)

		# Error out if the regex did not match (invalid line). A stamp that can't run must
		# not let the authentication pass, also not one with a typo
		if not regex_result:
			print(S(f"Error parsing rubberstamp rule: {rule}"))
			return False

		type = regex_result.group(1)

		# Error out if the stamp name in the rule is not a file
		if type not in installed_stamps:
			print(S(f"Stamp not installed: {type}"))
			return False

		# Load the stamp as a module of this package, so its relative imports work
		module = import_module("." + type, __package__)

		# Try to get the class with the same name
		try:
			constructor = getattr(module, type)
		except AttributeError:
			print(S(f"Stamp error: Class {type} not found"))
			return False

		# Init the class and set common values
		instance = constructor()
		instance.verbose = verbose
		instance.config = config
		instance._send_to_ui = send_to_ui
		instance.cancel_event = cancel_event
		instance.keep_alive = keep_alive
		instance.opencv = opencv

		# Set some opensv shorthands
		instance.video_capture = opencv["video_capture"]
		instance.face_detector = opencv["face_detector"]
		instance.pose_predictor = opencv["pose_predictor"]
		instance.clahe = opencv["clahe"]

		# Parse and set the 2 required options for all rubberstamps
		try:
			timeout = float(re.sub("[a-zA-Z]", "", regex_result.group(2)))
		except ValueError:
			print(S(f"Error parsing rubberstamp rule: {rule}"))
			return False
		instance.options = {
			"timeout": timeout,
			"failsafe": regex_result.group(3) != "faildeadly",
		}

		# Try to get the class do declare its other config variables
		try:
			instance.declare_config()
		except Exception:
			print(S("Internal error in rubberstamp configuration declaration:"))

			import traceback

			traceback.print_exc()
			# A stamp that can't run must not let the authentication pass
			return False

		# Split the optional arguments at the end of the rule by spaces
		raw_options = regex_result.group(4).split()

		# For each of those aoptional arguments
		for option in raw_options:
			# Get the key to the left, and the value to the right of the equal sign
			key, separator, value = option.partition("=")
			if not separator:
				print("Invalid argument for rubberstamp " + type + ": " + option)
				return False

			# Error out if a key has been set that was not declared by the module before
			# (the timeout and the failure mode are not arguments, they come before them)
			if key not in instance.options or key in ("timeout", "failsafe"):
				print("Unknown config option for rubberstamp " + type + ": " + key)
				return False

			# Convert the argument string to an int or float if the declared option has that type
			try:
				if isinstance(instance.options[key], int):
					value = int(value)
				elif isinstance(instance.options[key], float):
					value = float(value)
			except ValueError:
				print("Invalid argument for rubberstamp " + type + ": " + option)
				return False

			instance.options[key] = value

		if verbose:
			print('Stamp "' + type + '" options parsed:')
			print(instance.options)

		stamps.append((type, instance))

	# Run the stamps one by one
	for type, instance in stamps:
		if verbose:
			print('Executing stamp "' + type + '"')

		# Make the stamp fail by default
		result = False

		# Don't start a stamp if the authentication is already cancelled
		instance.check_cancelled()

		# Run the stamp code
		try:
			result = instance.run()
		except StampCancelled:
			raise
		except Exception:
			print(S("Internal error in rubberstamp:"))

			import traceback

			traceback.print_exc()
			# A stamp that can't run must not let the authentication pass
			result = False

		if verbose:
			print('Stamp "' + type + '" returned: ' + str(result))

		# Abort authentication if the stamp returned false
		if result is False:
			if verbose:
				print("Authentication aborted by rubber stamp")
			return False

	# This is outside the for loop, so we've run all the rules
	if verbose:
		print("All rubberstamps processed, authentication successful")

	return True

from ..i18n import S  # type: ignore

# Import the root rubberstamp class
from . import RubberStamp


class hotkey(RubberStamp):
	pressed_key = "none"

	def declare_config(self):
		"""Set the default values for the optional arguments"""
		self.options["abort_key"] = "esc"
		self.options["confirm_key"] = "enter"

	def run(self):
		"""Wait for the user to press a hotkey"""
		time_left = self.options["timeout"]
		time_string = (
			S("Aborting authorisation in {}")
			if self.options["failsafe"]
			else S("Authorising in {}")
		)

		# Set the ui to default strings
		self.send_to_ui(
			message=time_string.format(int(time_left)),
			subtext=S("Press {} to abort, {} to authorise").format(
				self.options["abort_key"], self.options["confirm_key"]
			),
		)

		# Try to import the keyboard module and tell the user to install the module if that fails
		try:
			import keyboard
		except Exception:
			print("\nMissing module for rubber stamp keyboard!")
			print("Please run:")
			print("\t pip3 install keyboard")
			return False

		# Register hotkeys with the kernel
		keyboard.add_hotkey(self.options["abort_key"], self.on_key, args=["abort"])
		keyboard.add_hotkey(self.options["confirm_key"], self.on_key, args=["confirm"])

		# Always unregister them again, also when the stamp gets cancelled
		try:
			return self.wait_for_key(time_left, time_string)
		finally:
			keyboard.remove_hotkey(self.options["abort_key"])
			keyboard.remove_hotkey(self.options["confirm_key"])

	def wait_for_key(self, time_left, time_string):
		"""Count down until a hotkey is pressed or the time is over"""

		# While we have not hit our timeout yet
		while time_left > 0:
			# Remove 0.1 seconds from the timer, as that's how long we sleep
			time_left -= 0.1
			# Update the ui with the new time
			self.send_to_ui(message=time_string.format(str(int(time_left) + 1)))

			# If the abort key was pressed while the loop was sleeping
			if self.pressed_key == "abort":
				# Set the ui to confirm the abort
				self.send_to_ui(message=S("Authentication aborted"), subtext="")

				# Exit
				self.sleep(1)
				return False

			# If confirm has pressed, return that auth can continue
			elif self.pressed_key == "confirm":
				return True

			# If no key has been pressed, wait for a bit and check again
			self.sleep(0.1)

		# When our timeout hits, either abort or continue based on failsafe of faildeadly
		return not self.options["failsafe"]

	def on_key(self, type):
		"""Called when the user presses a key"""
		self.pressed_key = type

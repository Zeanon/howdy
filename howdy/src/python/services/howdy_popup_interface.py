import time

from dbus_next.service import ServiceInterface, signal


POPUP_INTERFACE = "@popup_interface@"

# How long the popup may go without a signal before its text is sent again, it hides
# itself after a second
POPUP_KEEPALIVE = 0.4


class UIInterface(ServiceInterface):
	def __init__(self):
		super().__init__(POPUP_INTERFACE)
		# The texts the popup of a user shows, every signal is a D-Bus message
		self.last_state = {}

	def update_message(self, target_uid, message):
		"""Send UpdateMessage, unless the popup already shows this text"""
		state = self.last_state.get(target_uid)
		if state is not None:
			if state["message"] == message:
				return
			state["message"] = message
			state["sent"] = time.monotonic()
		self.UpdateMessage(target_uid, message)

	def update_subtext(self, target_uid, subtext):
		"""Send UpdateSubtext, unless the popup already shows this text"""
		state = self.last_state.get(target_uid)
		if state is not None:
			if state["subtext"] == subtext:
				return
			state["subtext"] = subtext
			state["sent"] = time.monotonic()
		self.UpdateSubtext(target_uid, subtext)

	def keep_alive(self, target_uid):
		"""The popup hides itself after a while without a signal: call this while the
		authentication runs, also when there is no new text. It is rate limited."""
		state = self.last_state.get(target_uid)
		if state is None or time.monotonic() - state["sent"] < POPUP_KEEPALIVE:
			return
		state["sent"] = time.monotonic()
		# The text it already shows: the popup only has to hear from the daemon
		self.UpdateMessage(target_uid, state["message"])

	@signal()
	def Show(
		self,
		target_uid: "u",  # type: ignore
		message: "s",  # type: ignore
	) -> "us":  # type: ignore
		# The popup starts with the text of Show and no subtext
		self.last_state[target_uid] = {
			"message": message,
			"subtext": None,
			"sent": time.monotonic(),
		}
		return [target_uid, message]

	@signal()
	def UpdateMessage(
		self,
		target_uid: "u",  # type: ignore
		message: "s",  # type: ignore
	) -> "us":  # type: ignore
		return [target_uid, message]

	@signal()
	def UpdateSubtext(
		self,
		target_uid: "u",  # type: ignore
		subtext: "s",  # type: ignore
	) -> "us":  # type: ignore
		return [target_uid, subtext]

	@signal()
	def Close(self, target_uid: "u") -> "u":  # type: ignore
		self.last_state.pop(target_uid, None)
		return target_uid

"""A stand-in for fprintd on the private bus, for the test of the PAM module.

The mode is read from the file in FPRINTD_MODE_FILE at every call, so it can
change while the stand-in runs:
  no-prints   VerifyStart fails, the user has no enrolled fingerprints
  no-device   there is no reader
  match       the finger matches a second after VerifyStart
  wait        VerifyStart works, nobody touches the reader
  slow        like wait, but Claim takes 0.6 seconds, as a reader that is slow to open
  stuck-claim like wait, but Claim does not answer (for 30 seconds), as a reader that is stuck
FPRINTD_REPLACE=1 takes the name over from a stand-in that runs already (the owner of the name
changes at once, as for a service that is restarted faster than it is noticed).
"""

import asyncio
import os

from dbus_next import BusType, DBusError, Message, NameFlag, Variant
from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method, signal

DEVICE = "/net/reactivated/Fprint/Device/0"


def mode():
	with open(os.environ["FPRINTD_MODE_FILE"]) as handle:
		return handle.read().strip()


class Manager(ServiceInterface):
	def __init__(self):
		super().__init__("net.reactivated.Fprint.Manager")

	@method()
	def GetDefaultDevice(self) -> "o":  # type: ignore
		if mode() == "no-device":
			raise DBusError("net.reactivated.Fprint.Error.NoSuchDevice", "No device")
		return DEVICE


class Device(ServiceInterface):
	def __init__(self):
		super().__init__("net.reactivated.Fprint.Device")

	@method()
	async def Claim(self, username: "s"):  # type: ignore
		if mode() == "slow":
			await asyncio.sleep(0.6)
		if mode() == "stuck-claim":
			await asyncio.sleep(30)

	@method()
	def Release(self):
		pass

	@method()
	def VerifyStart(self, finger: "s"):  # type: ignore
		if mode() == "no-prints":
			raise DBusError(
				"net.reactivated.Fprint.Error.NoEnrolledPrints", "No enrolled prints"
			)
		if mode() == "match":
			asyncio.get_running_loop().call_later(
				1, self.VerifyStatus, "verify-match", True
			)

	@method()
	def VerifyStop(self):
		pass

	@signal()
	def VerifyStatus(self, result: "s", done: "b") -> "sb":  # type: ignore
		return [result, done]


def properties(msg):
	"""The property is called scan-type, a name dbus-next does not accept for a property"""
	if (
		msg.interface == "org.freedesktop.DBus.Properties"
		and msg.member == "Get"
		and msg.body == ["net.reactivated.Fprint.Device", "scan-type"]
	):
		return Message.new_method_return(msg, "v", [Variant("s", "press")])
	return None


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	bus.add_message_handler(properties)
	bus.export("/net/reactivated/Fprint/Manager", Manager())
	bus.export(DEVICE, Device())
	# The first one lets itself be replaced, the one that replaces takes over
	if os.environ.get("FPRINTD_REPLACE"):
		flags = NameFlag.REPLACE_EXISTING
	else:
		flags = NameFlag.ALLOW_REPLACEMENT
	await bus.request_name("net.reactivated.Fprint", flags)
	await asyncio.get_running_loop().create_future()


asyncio.run(main())

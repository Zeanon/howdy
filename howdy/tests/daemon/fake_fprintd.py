"""A stand-in for fprintd on the private bus, for the test of the PAM module.

The mode is read from the file in FPRINTD_MODE_FILE at every call, so it can
change while the stand-in runs:
  no-prints   VerifyStart fails, the user has no enrolled fingerprints
  no-device   there is no reader
  match       the finger matches a second after VerifyStart
  wait        VerifyStart works, nobody touches the reader
  slow        like wait, but Claim takes 0.6 seconds, as a reader that is slow to open
"""

import asyncio
import os
import time

from dbus_next import BusType, DBusError, Message, Variant
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
	def Claim(self, username: "s"):  # type: ignore
		if mode() == "slow":
			time.sleep(0.6)

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
	await bus.request_name("net.reactivated.Fprint")
	await asyncio.get_running_loop().create_future()


asyncio.run(main())

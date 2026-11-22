# A stand-in for polkit on the private test bus.
# Usage: fake_polkit.py <mode file> <log file>
#
# The mode file contains allow, deny, challenge or slow (allow after five seconds) and is read
# on every call.
# Every call is logged as "<bus name of the client> <action id> <flags>".
import sys
import asyncio

from dbus_next import BusType
from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method

mode_file, log_file = sys.argv[1], sys.argv[2]


class Authority(ServiceInterface):
	def __init__(self):
		super().__init__("org.freedesktop.PolicyKit1.Authority")

	@method()
	async def CheckAuthorization(
		self,
		subject: "(sa{sv})",
		action_id: "s",
		details: "a{ss}",
		flags: "u",
		cancellation_id: "s",
	) -> "(bba{ss})":
		kind, data = subject
		with open(log_file, "a") as log:
			log.write(f"{kind} {data['name'].value} {action_id} {flags}\n")

		with open(mode_file) as mode_data:
			mode = mode_data.read().strip()

		# slow: the answer comes after a few seconds, like a user who types the password
		if mode == "slow":
			await asyncio.sleep(5)
			return [True, False, {}]

		# (is_authorized, is_challenge, details)
		return [mode == "allow", mode == "challenge", {}]


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	bus.export("/org/freedesktop/PolicyKit1/Authority", Authority())
	await bus.request_name("org.freedesktop.PolicyKit1")
	await asyncio.get_running_loop().create_future()


asyncio.run(main())

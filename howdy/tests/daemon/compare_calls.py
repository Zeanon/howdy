# Calls Compare of the management daemon several times on one connection.
# Usage: compare_calls.py <count> <metric> <number> ...
#
# Prints the reply of every call, or the name of its error, one per line. The environment
# variable COMPARE_PAUSE is a pause in seconds between the calls. With COMPARE_KEEPALIVE (seconds)
# KeepAlive is called that often during the pause, its answer is printed as "keepalive True/False".
# The face is the list of the numbers.
import os
import sys
import asyncio

from dbus_next import BusType, Message, MessageType
from dbus_next.aio import MessageBus

count, metric, face = int(sys.argv[1]), sys.argv[2], [float(n) for n in sys.argv[3:]]


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	pause = float(os.environ.get("COMPARE_PAUSE", "0"))
	keepalive = float(os.environ.get("COMPARE_KEEPALIVE", "0"))
	for i in range(count):
		if i:
			left = pause
			while left > 0:
				step = min(keepalive, left) if keepalive else left
				await asyncio.sleep(step)
				left -= step
				if keepalive:
					kept = await bus.call(
						Message(
							destination="org.boltgolt.howdy.management",
							path="/org/boltgolt/howdy/Management",
							interface="org.boltgolt.howdy.Management",
							member="KeepAlive",
						)
					)
					print("keepalive", kept.body[0])
		reply = await bus.call(
			Message(
				destination="org.boltgolt.howdy.management",
				path="/org/boltgolt/howdy/Management",
				interface="org.boltgolt.howdy.Management",
				member="Compare",
				signature="sad",
				body=[metric, face],
			)
		)
		if reply.message_type == MessageType.ERROR:
			print(reply.error_name)
		else:
			print(reply.body)
	bus.disconnect()


asyncio.run(main())

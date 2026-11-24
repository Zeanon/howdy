# Calls Compare of the management daemon several times on one connection.
# Usage: compare_calls.py <count> <metric> <number> ...
#
# Prints the reply of every call, or the name of its error, one per line. The environment
# variable COMPARE_PAUSE is a pause in seconds between the calls, COMPARE_CONCURRENT=1 sends all the
# calls at once.
# The face is the list of the numbers.
import os
import sys
import asyncio

from dbus_next import BusType, Message, MessageType
from dbus_next.aio import MessageBus

count, metric, face = int(sys.argv[1]), sys.argv[2], [float(n) for n in sys.argv[3:]]


def compare_message():
	return Message(
		destination="org.boltgolt.howdy.management",
		path="/org/boltgolt/howdy/Management",
		interface="org.boltgolt.howdy.Management",
		member="Compare",
		signature="sad",
		body=[metric, face],
	)


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	if os.environ.get("COMPARE_CONCURRENT"):
		replies = await asyncio.gather(
			*[bus.call(compare_message()) for _ in range(count)]
		)
		for reply in replies:
			print(
				reply.error_name
				if reply.message_type == MessageType.ERROR
				else reply.body
			)
		bus.disconnect()
		return
	pause = float(os.environ.get("COMPARE_PAUSE", "0"))
	for i in range(count):
		if i and pause:
			await asyncio.sleep(pause)
		reply = await bus.call(compare_message())
		if reply.message_type == MessageType.ERROR:
			print(reply.error_name)
		else:
			print(reply.body)
	bus.disconnect()


asyncio.run(main())

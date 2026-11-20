import os

from dbus_next import Message, MessageType


class CallerError(RuntimeError):
	"""The caller of a method could not be identified"""


class MissingSenderError(CallerError):
	"""The method call has no sender"""


async def query_caller_uid(bus, sender):
	"""The uid of the sender of a message, as the bus knows it and not as the
	sender claims"""
	if not sender:
		raise MissingSenderError("Missing D-Bus sender")

	reply = await bus.call(
		Message(
			destination="org.freedesktop.DBus",
			path="/org/freedesktop/DBus",
			interface="org.freedesktop.DBus",
			member="GetConnectionUnixUser",
			signature="s",
			body=[sender],
		)
	)
	if reply.message_type == MessageType.ERROR:
		raise CallerError(f"Could not identify the caller: {reply.body[0]}")
	return reply.body[0]


def is_privileged(uid, trust_own_user=False):
	"""Root may do everything. With trust_own_user the user the daemon runs as
	counts like root, for the daemons that do not run as root."""
	return uid == 0 or (trust_own_user and uid == os.geteuid())


def may_access(caller_uid, owner_uid, trust_own_user=False):
	"""Whether the caller may act on something of the owner: privileged callers
	on everything, everybody else only on their own"""
	return is_privileged(caller_uid, trust_own_user) or caller_uid == owner_uid

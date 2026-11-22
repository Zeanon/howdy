import os

from dbus_next import Message, MessageType


class RateLimiter:
	"""A token bucket for each key (the user id of a caller). A bucket holds up to `burst`
	tokens and is filled `rate_per_second` tokens a second, every use takes one. The time is an
	argument so a test does not have to wait."""

	def __init__(self, rate_per_second, burst):
		self.rate = rate_per_second
		self.burst = burst
		self.buckets = {}

	def allow(self, key, now):
		"""Whether the key may do it now. A refused try takes nothing."""
		tokens, last = self.buckets.get(key, (self.burst, now))
		tokens = min(self.burst, tokens + max(now - last, 0) * self.rate)
		if tokens < 1:
			self.buckets[key] = (tokens, now)
			return False
		self.buckets[key] = (tokens - 1, now)
		return True


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

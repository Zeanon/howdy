import pwd

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType, Variant

from ..caller import may_access

POLKIT_BUS = "org.freedesktop.PolicyKit1"
POLKIT_PATH = "/org/freedesktop/PolicyKit1/Authority"
POLKIT_INTERFACE = "org.freedesktop.PolicyKit1.Authority"

POLKIT_ACTION = "@management_interface@"
POLKIT_ALLOW_USER_INTERACTION = 1


async def check_authorization(
	bus=None, bus_name=None, caller_uid=None, target_user=None
):
	# If we have no Bus, create one and call ourselves with that
	if bus is None:
		bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
		try:
			await check_authorization(
				bus=bus,
				bus_name=bus_name,
				caller_uid=caller_uid,
				target_user=target_user,
			)
		finally:
			bus.disconnect()
		return

	if bus_name is None:
		bus_name = bus.unique_name

	if caller_uid is not None or target_user is not None:
		try:
			target_uid = pwd.getpwnam(target_user).pw_uid
		except KeyError:
			raise ValueError(f"Unknown user: {target_user}")
		if not may_access(caller_uid, target_uid):
			raise PermissionError(
				f"Not authorized to modify models for user {target_user}"
			)

	reply = await bus.call(
		Message(
			destination=POLKIT_BUS,
			path=POLKIT_PATH,
			interface=POLKIT_INTERFACE,
			member="CheckAuthorization",
			signature="(sa{sv})sa{ss}us",
			body=[
				[
					"system-bus-name",
					{
						"name": Variant("s", bus_name),
					},
				],
				POLKIT_ACTION,
				{},
				POLKIT_ALLOW_USER_INTERACTION,
				"",
			],
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(f"Polkit authorization failed: {reply.body}")

	if reply.signature != "(bba{ss})":
		raise RuntimeError(f"Unexpected Polkit reply signature: {reply.signature}")

	authorized, challenge, details = reply.body[0]

	if not authorized:
		raise PermissionError(
			"Polkit authorization denied"
			+ (" (authentication required)" if challenge else "")
		)

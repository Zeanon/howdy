# Clear all models by deleting the whole file

# Import required modules
import os
import sys
import asyncio
import builtins

from ..i18n import S
from .auth import check_authorization

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType


MANAGEMENT_BUS = "@management_bus@"
MANAGEMENT_PATH = "@management_path@"
MANAGEMENT_INTERFACE = "@management_interface@"


def exit(reason):
	if builtins.howdy_args.gui:
		raise Exception(reason)
	print(reason)
	sys.exit(1)


async def clear_models(bus):
	signature = "s" if builtins.howdy_args.user else ""
	body = [builtins.howdy_args.user] if builtins.howdy_args.user else []

	reply = await bus.call(
		Message(
			destination=MANAGEMENT_BUS,
			path=MANAGEMENT_PATH,
			interface=MANAGEMENT_INTERFACE,
			member="Clear",
			signature=signature,
			body=body,
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(reply.body[0])

	if not reply.body[0]:
		raise RuntimeError("Failed to clear models")


def run():
	asyncio.run(_run())


async def _run():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	try:
		# Get the passed user
		user = builtins.howdy_args.user
		try:
			await check_authorization(
				bus=bus, caller_uid=os.geteuid(), target_user=user
			)
		except Exception as exc:
			exit(f"Authorization failed: {exc}")

		if not builtins.howdy_args.y:
			print(S("This will clear all models for ") + user)
			ans = input(S("Do you want to continue [y/N]: "))

			if ans.lower() != "y":
				print(S('\nInterpreting as a "NO", aborting'))
				sys.exit(1)

		try:
			await clear_models(bus)
		except Exception as exc:
			print(S("Failed to clear models: {}").format(exc))
			sys.exit(1)

		print(S("\nModels cleared"))
	finally:
		bus.disconnect()

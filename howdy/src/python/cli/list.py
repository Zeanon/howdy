# List all models for a user

# Import required modules
import os
import sys
import json
import time
import asyncio
import builtins

from ..i18n import S
from .auth import check_authorization

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType


MANAGEMENT_BUS = "@management_bus@"
MANAGEMENT_PATH = "@management_path@"
MANAGEMENT_INTERFACE = "@management_interface@"


async def list_models(bus):
	signature = "s" if builtins.howdy_args.user else ""
	body = [builtins.howdy_args.user] if builtins.howdy_args.user else []

	reply = await bus.call(
		Message(
			destination=MANAGEMENT_BUS,
			path=MANAGEMENT_PATH,
			interface=MANAGEMENT_INTERFACE,
			member="List",
			signature=signature,
			body=body,
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(reply.body[0])

	return json.loads(reply.body[0])


def run(gui=False):
	return asyncio.run(_run(gui))


async def _run(gui=False):
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	try:
		user = builtins.howdy_args.user
		try:
			await check_authorization(
				bus=bus, caller_uid=os.geteuid(), target_user=user
			)
		except Exception as exc:
			print(f"Authorization failed: {exc}")
			sys.exit(1)

		# Ich möchte den zugriff auf die model datei in einen dbus daemon verschieben
		# Check if the models file has been created yet
		try:
			encodings = await list_models(bus)
		except Exception as exc:
			if gui:
				return []

			print(S(f"Failed to retrieve face models: {exc}"))
			sys.exit(1)

		if not encodings:
			if gui:
				return []

			if not builtins.howdy_args.plain:
				print(S(f"No face model known for the user {user}, please run:"))
				print("\n\thowdy add\n")

			sys.exit(1)
		# Hier soll der return wert vom dbus aufruf zurück kommen
		if gui:
			return [
				(
					enc["id"],
					time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(enc["time"])),
					enc["label"],
				)
				for enc in encodings
			]

		# Print a header if we're not in plain mode
		if not builtins.howdy_args.plain:
			print(S(f"Known face models for {user}:"))
			print("\n\033[1;29m" + S("ID  Date                 Label\033[0m"))

		# Loop through all encodings and print info about them
		for enc in encodings:
			# Start with the id
			print(str(enc["id"]), end="")

			# Add comma for machine reading
			if builtins.howdy_args.plain:
				print(",", end="")
			# Print padding spaces after the id for a nice layout
			else:
				print((4 - len(str(enc["id"]))) * " ", end="")

			# Format the time as ISO in the local timezone
			print(
				time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(enc["time"])), end=""
			)

			# Separate with commas again for machines, spaces otherwise
			print("," if builtins.howdy_args.plain else "  ", end="")

			# End with the label
			print(enc["label"])

		# Add a closing enter
		print()
	finally:
		bus.disconnect()

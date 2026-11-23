# Remove a encoding from the models file

# Import required modules
import os
import sys
import asyncio
import builtins

from ..i18n import S
from .list import list_models
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


def say(line):
	if not builtins.howdy_args.gui:
		print(line)


async def remove_model(bus, id, expected_label, expected_time):
	"""The daemon only removes the model if it still has the label and time the caller saw
	it with: the ids move up when a model is removed, an old list would hit another one"""
	signature = "ssss" if builtins.howdy_args.user else "sss"
	body = [builtins.howdy_args.user] if builtins.howdy_args.user else []
	body += [str(id), expected_label, str(expected_time)]

	reply = await bus.call(
		Message(
			destination=MANAGEMENT_BUS,
			path=MANAGEMENT_PATH,
			interface=MANAGEMENT_INTERFACE,
			member="Remove",
			signature=signature,
			body=body,
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(reply.body[0])

	if not reply.body[0]:
		raise RuntimeError("Failed to remove model")


def run(expected_label=None, expected_time=None):
	"""expected_label, expected_time: the label and time (seconds) the caller (the window)
	shows for the model, the ones found in the list are used without them"""
	asyncio.run(_run(expected_label, expected_time))


async def _run(expected_label=None, expected_time=None):
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	try:
		user = builtins.howdy_args.user
		try:
			await check_authorization(
				bus=bus, caller_uid=os.geteuid(), target_user=user
			)
		except Exception as exc:
			exit(f"Authorization failed: {exc}")

		# Check if enough arguments have been passed
		if not builtins.howdy_args.arguments:
			reason = S(
				"Please add the ID of the model you want to remove as an argument\n"
			)
			reason += S("For example:\n")
			reason += "\thowdy remove 0\n"
			reason += S("You can find the IDs by running:\n")
			reason += "\thowdy list"
			exit(reason)

		# Try to load the models file and abort if the user does not have it yet
		try:
			encodings = await list_models(bus)
		except FileNotFoundError:
			reason = S(f"No face model known for the user {user}, please run:\n")
			reason += "\thowdy add"
			exit(reason)

		# Tracks if a encoding with that id has been found
		found = False

		# Get the ID from the cli arguments
		id = builtins.howdy_args.arguments[0]

		# Loop though all encodings and check if they match the argument
		for enc in encodings:
			if str(enc["id"]) == id:
				# Only ask the user if there's no -y flag
				if not builtins.howdy_args.y:
					# Double check with the user
					print(
						S(
							f'This will remove the model called "{enc["label"]}" for {user}'
						)
					)
					ans = input(S("Do you want to continue [y/N]: "))

					# Abort if the answer isn't yes
					if ans.lower() != "y":
						print(S('\nInterpreting as a "NO", aborting'))
						sys.exit(1)

					# Add a padding empty  line
					print()

				# Mark as found and print an enter
				found = True
				found_label = enc["label"]
				found_time = int(enc["time"])
				break

		# Abort if no matching id was found
		if not found:
			exit(S(f"No model with ID {id} exists for {user}"))

		try:
			await remove_model(
				bus,
				id,
				expected_label if expected_label is not None else found_label,
				expected_time if expected_time is not None else found_time,
			)
		except Exception as exc:
			exit(f"Failed to remove face model: {exc}")

		if len(encodings) == 1:
			say(S("Removed last model, howdy disabled for user"))
		else:
			say(S(f"Removed model {id}"))
	finally:
		bus.disconnect()

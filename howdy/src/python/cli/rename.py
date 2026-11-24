# Give a model another name

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


async def rename_model(bus, id, expected_label, expected_time, label):
	"""The daemon only renames the model if it still has the label and time the caller saw
	it with: the ids move up when a model is removed, an old list would hit another one"""
	signature = "sxsxs" if builtins.howdy_args.user else "xsxs"
	body = [builtins.howdy_args.user] if builtins.howdy_args.user else []
	body += [int(id), expected_label, int(expected_time), label]

	reply = await bus.call(
		Message(
			destination=MANAGEMENT_BUS,
			path=MANAGEMENT_PATH,
			interface=MANAGEMENT_INTERFACE,
			member="Rename",
			signature=signature,
			body=body,
		)
	)

	if reply.message_type == MessageType.ERROR:
		raise RuntimeError(reply.body[0])

	if not reply.body[0]:
		raise RuntimeError("Failed to rename model")


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
				"Please add the ID of the model you want to rename as an argument\n"
			)
			reason += S("For example:\n")
			reason += "\thowdy rename 0\n"
			reason += '\thowdy rename 0 "Living room"\n'
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

		# Get the ID from the cli arguments and look for the model
		id = builtins.howdy_args.arguments[0]
		found = next((enc for enc in encodings if str(enc["id"]) == id), None)

		# Abort if no matching id exists
		if found is None:
			exit(S(f"No model with ID {id} exists for {user}"))

		# Get the new name from the cli arguments if provided, otherwise ask for it
		if len(builtins.howdy_args.arguments) > 1:
			label = builtins.howdy_args.arguments[1]
		else:
			# Nobody can answer with -y, or without a TTY / with a closed stdin
			if builtins.howdy_args.y or not sys.stdin.isatty():
				exit(S("Please add the new name of the model as an argument"))
			try:
				label = input(
					S(f"Enter a new label for this model [{found['label']}]: ")
				)
			except EOFError:
				label = ""

			if label == "":
				label = found["label"]
			else:
				# The same limit as when adding a model
				label = label[:24]

		# Remove illegal characters
		if "," in label:
			say(S('NOTICE: Removing illegal character "," from model name'))
			label = label.replace(",", "")
		if label == "":
			exit(S("The new name of the model is empty"))

		try:
			await rename_model(
				bus,
				int(found["id"]),
				expected_label if expected_label is not None else found["label"],
				expected_time if expected_time is not None else int(found["time"]),
				label,
			)
		except Exception as exc:
			exit(f"Failed to rename face model: {exc}")

		say(S(f"Renamed model {id}"))
	finally:
		bus.disconnect()

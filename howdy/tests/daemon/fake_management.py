# A stand-in for the management daemon on the private test bus.
# Usage: fake_management.py <models file> <mode file> <log file>
#
# The models are kept in a file. The mode file is read on every call:
#   allow          everything works (default)
#   deny           every call fails with NotAuthorized, like a refused polkit
#   list-error     List fails
#   add-error      Add fails
#   remove-error   Remove fails
# Every call is logged as "<bus name of the caller> <method> <user>".
import sys
import json
import asyncio

from dbus_next import BusType, Message, MessageType
from dbus_next.aio import MessageBus

models_file, mode_file, log_file = sys.argv[1:4]

PATH = "/org/boltgolt/howdy/Management"
INTERFACE = "org.boltgolt.howdy.Management"


def load():
	try:
		with open(models_file) as data:
			return json.load(data)
	except FileNotFoundError:
		return []


def save(models):
	with open(models_file, "w") as data:
		json.dump(models, data)


def mode():
	try:
		with open(mode_file) as data:
			return data.read().strip() or "allow"
	except FileNotFoundError:
		return "allow"


def error(msg, text, name=None):
	return Message.new_error(msg, name or f"{INTERFACE}.{msg.member}.Error", text)


def handler(msg):
	if (
		msg.message_type != MessageType.METHOD_CALL
		or msg.path != PATH
		or msg.interface != INTERFACE
	):
		return None

	with open(log_file, "a") as log:
		log.write(f"{msg.sender} {msg.member} {msg.body[0] if msg.body else ''}\n")

	current = mode()
	if current == "deny":
		return error(msg, "Polkit authorization denied", f"{INTERFACE}.NotAuthorized")
	if current == "list-error" and msg.member == "List":
		return error(msg, "The model file is invalid")
	if current == "add-error" and msg.member == "Add":
		return error(msg, "The model could not be saved")
	if current == "remove-error" and msg.member == "Remove":
		return error(msg, "No model with ID")

	models = load()

	if msg.member == "List":
		shown = [
			{"time": m["time"], "label": m["label"], "id": m["id"]} for m in models
		]
		return Message.new_method_return(msg, "s", [json.dumps(shown)])

	if msg.member == "Add":
		model = json.loads(msg.body[1])
		model["id"] = models[-1]["id"] + 1 if models else 0
		models.append(model)
		save(models)
		return Message.new_method_return(msg, "b", [True])

	if msg.member == "Remove":
		remaining = [m for m in models if str(m["id"]) != msg.body[1]]
		if len(remaining) == len(models):
			return error(msg, f"No model with ID {msg.body[1]} exists")
		save(remaining)
		return Message.new_method_return(msg, "b", [True])

	return None


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	bus.add_message_handler(handler)
	await bus.request_name("org.boltgolt.howdy.management")
	await asyncio.get_running_loop().create_future()


asyncio.run(main())

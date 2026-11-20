import os
import pwd
import json
import syslog
import asyncio
import traceback

from . import paths_factory
from .file_util import read_trusted_file, write_file_atomic
from .cli.auth import check_authorization
from .caller import MissingSenderError, query_caller_uid

from dbus_next.aio import MessageBus
from dbus_next import Message, MessageType, BusType


MANAGEMENT_BUS = "@management_bus@"
MANAGEMENT_PATH = "@management_path@"
MANAGEMENT_INTERFACE = "@management_interface@"

# The names of the errors of the management interface
# A refused authorization has its own name, so that clients can tell it from
# other failures
NOT_AUTHORIZED_ERROR = "@management_interface@.NotAuthorized"
ADD_ERROR = "@management_interface@.Add.Error"
LIST_ERROR = "@management_interface@.List.Error"
REMOVE_ERROR = "@management_interface@.Remove.Error"
CLEAR_ERROR = "@management_interface@.Clear.Error"


POLKIT_BUS = "org.freedesktop.PolicyKit1"
POLKIT_PATH = "/org/freedesktop/PolicyKit1/Authority"
POLKIT_INTERFACE = "org.freedesktop.PolicyKit1.Authority"

POLKIT_ACTION = "@management_interface@"
POLKIT_ALLOW_USER_INTERACTION = 1


IDLE_TIMEOUT = 30


class NotAuthorized(Exception):
	"""The caller may not manage the face models"""


class ManagementInterface:
	def __init__(self, bus):
		self.bus = bus
		self.tasks = set()
		self.user_locks = {}

		self.idle_handle = None
		self.shutdown_event = asyncio.Event()
		self.stopping = False

		self.methods = {
			"Add": ("s", self.add),
			"List": ("", self.list),
			"Remove": ("s", self.remove),
			"Clear": ("", self.clear),
		}

	def cancel_idle_timer(self):
		if self.idle_handle is not None:
			self.idle_handle.cancel()
			self.idle_handle = None

	def reset_idle_timer(self):
		if self.stopping or self.tasks:
			return

		self.cancel_idle_timer()

		loop = asyncio.get_running_loop()
		self.idle_handle = loop.call_later(
			IDLE_TIMEOUT,
			self.on_idle_timeout,
		)

	def on_idle_timeout(self):
		self.idle_handle = None

		if self.tasks:
			return

		self.stopping = True
		self.shutdown_event.set()

	def handle_message(self, msg):
		if (
			msg.message_type != MessageType.METHOD_CALL
			or msg.path != MANAGEMENT_PATH
			or msg.interface != MANAGEMENT_INTERFACE
		):
			return False

		method = self.methods.get(msg.member)

		if method is None:
			return False

		signature, callback = method

		if msg.signature not in (signature, "s" + signature):
			return False

		if self.stopping:
			return False

		self.cancel_idle_timer()

		task = asyncio.create_task(callback(msg))
		self.tasks.add(task)

		def task_finished(task):
			self.tasks.discard(task)

			if not self.tasks:
				self.reset_idle_timer()

		task.add_done_callback(task_finished)

		return True

	async def get_caller(self, msg, min_len):
		try:
			caller_uid = await query_caller_uid(self.bus, msg.sender)
		except MissingSenderError as exc:
			raise NotAuthorized(str(exc)) from exc

		if len(msg.body) > min_len:
			username = msg.body[0]
		else:
			username = pwd.getpwuid(caller_uid).pw_name

		try:
			await check_authorization(
				bus=self.bus,
				bus_name=msg.sender,
				caller_uid=caller_uid,
				target_user=username,
			)
		except PermissionError as exc:
			raise NotAuthorized(str(exc)) from exc
		return username

	def reply_failure(self, msg, exc, error, failure):
		"""Log a failed call and answer it with the error of its method, a refused
		authorization with NotAuthorized"""
		syslog.syslog(syslog.LOG_ERR, failure)
		syslog.syslog(syslog.LOG_ERR, traceback.format_exc())
		if isinstance(exc, NotAuthorized):
			error = NOT_AUTHORIZED_ERROR
		self.bus.send(Message.new_error(msg, error, str(exc)))

	async def add(self, msg):
		try:
			username = await self.get_caller(msg, 1)

			model = json.loads(msg.body[-1])

			if not isinstance(model, dict):
				raise TypeError("Model must be a dictionary")

			# Get or create a lock for this user
			lock = self.user_locks.setdefault(username, asyncio.Lock())

			async with lock:
				os.makedirs(
					paths_factory.user_models_dir_path(), mode=0o700, exist_ok=True
				)

				enc_file = paths_factory.user_model_path(username)
				try:
					encodings = json.loads(read_trusted_file(enc_file))
				except FileNotFoundError:
					encodings = []

				model["id"] = encodings[-1]["id"] + 1 if encodings else 0

				if not isinstance(encodings, list) or not all(
					isinstance(encoding, dict) for encoding in encodings
				):
					raise ValueError("Existing face model file is invalid")

				encodings.append(model)

				write_file_atomic(enc_file, json.dumps(encodings))

			self.bus.send(Message.new_method_return(msg, "b", [True]))

		except Exception as exc:
			self.reply_failure(msg, exc, ADD_ERROR, "Failed to add face model")

	async def list(self, msg):
		try:
			username = await self.get_caller(msg, 0)

			enc_file = paths_factory.user_model_path(username)

			try:
				encodings = json.loads(read_trusted_file(enc_file))
				for i in range(len(encodings)):
					encodings[i] = {
						"time": encodings[i]["time"],
						"label": encodings[i]["label"],
						"id": encodings[i]["id"],
					}
			except FileNotFoundError:
				encodings = []

			self.bus.send(Message.new_method_return(msg, "s", [json.dumps(encodings)]))

		except Exception as exc:
			self.reply_failure(msg, exc, LIST_ERROR, "Failed to list face models")

	async def remove(self, msg):
		try:
			username = await self.get_caller(msg, 1)
			model_id = msg.body[-1]

			lock = self.user_locks.setdefault(username, asyncio.Lock())

			async with lock:
				enc_file = paths_factory.user_model_path(username)

				try:
					encodings = json.loads(read_trusted_file(enc_file))
				except FileNotFoundError:
					raise FileNotFoundError(f"No face model known for user {username}")

				if not isinstance(encodings, list):
					raise ValueError("Face model file is invalid")

				remaining = [enc for enc in encodings if str(enc.get("id")) != model_id]

				if len(remaining) == len(encodings):
					raise ValueError(
						f"No model with ID {model_id} exists for {username}"
					)

				if len(remaining) == 0:
					os.remove(enc_file)
				else:
					write_file_atomic(enc_file, json.dumps(remaining))

				self.bus.send(Message.new_method_return(msg, "b", [True]))

		except Exception as exc:
			self.reply_failure(msg, exc, REMOVE_ERROR, "Failed to remove face model")

	async def clear(self, msg):
		try:
			username = await self.get_caller(msg, 0)

			lock = self.user_locks.setdefault(username, asyncio.Lock())

			async with lock:
				enc_file = paths_factory.user_model_path(username)

				if not os.path.isfile(enc_file):
					raise FileNotFoundError(
						f"{username} has no models or they have already been cleared"
					)

				os.remove(enc_file)

			self.bus.send(Message.new_method_return(msg, "b", [True]))

		except Exception as exc:
			self.reply_failure(msg, exc, CLEAR_ERROR, "Failed to clear face models")


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

	management_service = ManagementInterface(bus)

	bus.add_message_handler(management_service.handle_message)
	await bus.request_name(MANAGEMENT_BUS)

	syslog.syslog(
		syslog.LOG_INFO,
		"Howdy Management D-Bus service started",
	)

	# Start the idle timer
	management_service.reset_idle_timer()

	# Wait until the service becomes idle
	await management_service.shutdown_event.wait()

	syslog.syslog(
		syslog.LOG_INFO,
		"Howdy Management D-Bus service stopped after inactivity",
	)

	bus.disconnect()


if __name__ == "__main__":
	asyncio.run(main())

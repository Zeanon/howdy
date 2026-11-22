import os
import sys
import pwd
import time
import json
import syslog
import asyncio
import traceback

from . import paths_factory
from .file_util import (
	compare_face,
	encrypt_all_models,
	read_model_file,
	verify_models_key,
	write_model_file,
)
from .cli.auth import check_authorization
from .caller import MissingSenderError, RateLimiter, may_access, query_caller_uid

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
COMPARE_ERROR = "@management_interface@.Compare.Error"
KEEPALIVE_ERROR = "@management_interface@.KeepAlive.Error"
# Compare is called too often by this user, the client waits a moment and goes on
COMPARE_RATE_LIMITED_ERROR = "@management_interface@.Compare.RateLimited"


POLKIT_BUS = "org.freedesktop.PolicyKit1"
POLKIT_PATH = "/org/freedesktop/PolicyKit1/Authority"
POLKIT_INTERFACE = "org.freedesktop.PolicyKit1.Authority"

POLKIT_ACTION = "@management_interface@"
POLKIT_ALLOW_USER_INTERACTION = 1


IDLE_TIMEOUT = 30

# A keep-alive like the one of the authentication daemon: a connection that polkit authorized for
# Compare stays authorized for COMPARE_INITIAL_KEEPALIVE seconds, and every Compare or KeepAlive
# call of the connection extends that to COMPARE_KEEPALIVE_EXTENSION seconds from then. Without a
# call it is asked again. KeepAlive only extends what polkit allowed before, it allows nothing itself.
COMPARE_INITIAL_KEEPALIVE = 2
COMPARE_KEEPALIVE_EXTENSION = 1

# How often a user may call Compare: howdy test makes up to ten rounds a second with a call for
# every face, so this is a little more than it needs. Beyond that the answer is RateLimited, before
# polkit is asked or a model is read. The calls of a user are counted together (not for each
# connection: a program can open many), and KeepAlive does not count.
COMPARE_RATE_PER_SECOND = 30
COMPARE_BURST = 60


class NotAuthorized(Exception):
	"""The caller may not manage the face models"""


class ManagementInterface:
	def __init__(self, bus):
		self.bus = bus
		self.tasks = set()
		self.user_locks = {}
		# Bus names that were authorized for Compare, with the time it ends. A bus name
		# is never given to a second connection, so it can be remembered safely.
		self.compare_authorized = {}
		self.compare_limiter = RateLimiter(COMPARE_RATE_PER_SECOND, COMPARE_BURST)
		self.last_rate_limit_log = None

		self.idle_handle = None
		self.shutdown_event = asyncio.Event()
		self.stopping = False

		self.methods = {
			"Add": ("s", self.add),
			"List": ("", self.list),
			"Remove": ("s", self.remove),
			"Clear": ("", self.clear),
			"Compare": ("sad", self.compare),
			"KeepAlive": ("", self.keep_alive),
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

	async def get_caller_for_compare(self, msg, min_len):
		"""Compare is called for every frame of howdy test, so polkit is asked on the
		first call of a connection only. After that only root and the user are let in
		for as long as the connection keeps comparing."""
		now = time.monotonic()
		self.compare_authorized = {
			name: end for name, end in self.compare_authorized.items() if end > now
		}

		if msg.sender in self.compare_authorized:
			username = await self.get_caller_without_polkit(msg, min_len)
			self.compare_authorized[msg.sender] = now + COMPARE_KEEPALIVE_EXTENSION
			return username

		username = await self.get_caller(msg, min_len)
		self.compare_authorized[msg.sender] = (
			time.monotonic() + COMPARE_INITIAL_KEEPALIVE
		)
		return username

	async def get_caller_without_polkit(self, msg, min_len):
		"""Like get_caller, but only root and the user are let in, polkit is not asked"""
		try:
			caller_uid = await query_caller_uid(self.bus, msg.sender)
		except MissingSenderError as exc:
			raise NotAuthorized(str(exc)) from exc

		if len(msg.body) > min_len:
			username = msg.body[0]
		else:
			username = pwd.getpwuid(caller_uid).pw_name

		try:
			target_uid = pwd.getpwnam(username).pw_uid
		except KeyError:
			raise ValueError(f"Unknown user: {username}") from None
		if not may_access(caller_uid, target_uid):
			raise NotAuthorized(f"Not authorized to modify models for user {username}")
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
				models_dir = paths_factory.user_models_dir_path()
				os.makedirs(models_dir, mode=0o700, exist_ok=True)
				# Also for a directory of an older version that others could search
				os.chmod(models_dir, 0o700)

				enc_file = paths_factory.user_model_path(username)
				try:
					encodings = json.loads(read_model_file(enc_file, username))
				except FileNotFoundError:
					encodings = []

				model["id"] = encodings[-1]["id"] + 1 if encodings else 0

				if not isinstance(encodings, list) or not all(
					isinstance(encoding, dict) for encoding in encodings
				):
					raise ValueError("Existing face model file is invalid")

				encodings.append(model)

				write_model_file(enc_file, username, json.dumps(encodings))

			self.bus.send(Message.new_method_return(msg, "b", [True]))

		except Exception as exc:
			self.reply_failure(msg, exc, ADD_ERROR, "Failed to add face model")

	async def list(self, msg):
		try:
			username = await self.get_caller(msg, 0)

			enc_file = paths_factory.user_model_path(username)

			try:
				encodings = json.loads(read_model_file(enc_file, username))
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

	async def keep_alive(self, msg):
		"""Extend the authorization a connection got for Compare, see COMPARE_INITIAL_KEEPALIVE.
		It allows nothing itself. Reply: whether the connection was still authorized"""
		try:
			now = time.monotonic()
			end = self.compare_authorized.get(msg.sender)
			kept = end is not None and end > now
			if kept:
				self.compare_authorized[msg.sender] = now + COMPARE_KEEPALIVE_EXTENSION
			self.bus.send(Message.new_method_return(msg, "b", [kept]))

		except Exception as exc:
			self.reply_failure(
				msg, exc, KEEPALIVE_ERROR, "Failed to keep the authorization alive"
			)

	async def compare(self, msg):
		"""Compare a face with the models of the user, which are not given out.
		Reply: (found, value, label)"""
		try:
			# A user that calls Compare too often is told to slow down, before polkit is asked
			# (a program that does not stop would otherwise have a password prompt for every
			# call) or a model is read
			try:
				caller_uid = await query_caller_uid(self.bus, msg.sender)
			except MissingSenderError as exc:
				raise NotAuthorized(str(exc)) from exc
			now = time.monotonic()
			if not self.compare_limiter.allow(caller_uid, now):
				if (
					self.last_rate_limit_log is None
					or now - self.last_rate_limit_log > 60
				):
					self.last_rate_limit_log = now
					syslog.syslog(
						syslog.LOG_WARNING,
						f"Compare is called too often by the user {caller_uid}, slowing it down",
					)
				self.bus.send(
					Message.new_error(
						msg,
						COMPARE_RATE_LIMITED_ERROR,
						"Too many comparisons, please slow down",
					)
				)
				return

			username = await self.get_caller_for_compare(msg, 2)
			metric, face = msg.body[-2], msg.body[-1]

			try:
				models = json.loads(
					read_model_file(paths_factory.user_model_path(username), username)
				)
			except FileNotFoundError:
				models = []

			if not isinstance(models, list):
				raise TypeError("Face model file is invalid")

			match = compare_face(models, metric, face)
			body = [False, 0.0, ""] if match is None else [True, match[0], match[1]]
			self.bus.send(Message.new_method_return(msg, "bds", body))

		except Exception as exc:
			self.reply_failure(msg, exc, COMPARE_ERROR, "Failed to compare the face")

	async def remove(self, msg):
		try:
			username = await self.get_caller(msg, 1)
			model_id = msg.body[-1]

			lock = self.user_locks.setdefault(username, asyncio.Lock())

			async with lock:
				enc_file = paths_factory.user_model_path(username)

				try:
					encodings = json.loads(read_model_file(enc_file, username))
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
					write_model_file(enc_file, username, json.dumps(remaining))

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


def encrypt_models():
	"""--encrypt-models: encrypt the models that are still plain JSON, without the bus"""
	try:
		encrypted, already, failures = encrypt_all_models(
			str(paths_factory.user_models_dir_path())
		)
	except (OSError, ValueError, TypeError, RuntimeError) as exc:
		print(f"Failed to encrypt the models: {exc}", file=sys.stderr)
		return 1
	print(f"{encrypted} model file(s) encrypted, {already} already encrypted")
	for failure in failures:
		print(f"Not encrypted: {failure}", file=sys.stderr)
	return 1 if failures else 0


def verify_key():
	"""--verify-key: does the key on stdin (64 hex digits) decrypt the models?
	0 yes, 1 no, 3 nothing to check it against"""
	try:
		text = sys.stdin.readline().strip()
		if len(text) != 64:
			raise ValueError("the key must be 64 hex digits")
		matching, failures = verify_models_key(
			str(paths_factory.user_models_dir_path()), bytes.fromhex(text)
		)
	except (OSError, ValueError, RuntimeError) as exc:
		print(f"Failed to check the key: {exc}", file=sys.stderr)
		return 1
	for failure in failures:
		print(f"Does not decrypt: {failure}", file=sys.stderr)
	if failures:
		return 1
	if matching == 0:
		print("There is no encrypted model file to check the key against")
		return 3
	print(f"The key decrypts {matching} model file(s)")
	return 0


if __name__ == "__main__":
	if sys.argv[1:] == ["--verify-key"]:
		sys.exit(verify_key())
	if sys.argv[1:] == ["--encrypt-models"]:
		sys.exit(encrypt_models())
	if len(sys.argv) != 1:
		print(
			f"Usage: {sys.argv[0]} [--encrypt-models | --verify-key]", file=sys.stderr
		)
		sys.exit(2)
	asyncio.run(main())

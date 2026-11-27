"""Test of the Python daemons that needs no bus: the checks who calls them (the
common caller checks, the authentication daemon: who may see, cancel and clear a
request and who may start one, the management daemon with the polkit check of
the cli package), the retention of the snapshots and which model files are used.

Usage: python_callers.py <src/python dir> <work dir>

The sources are staged like the installation does, with the placeholders of meson
filled in. The bus is a stand-in that answers GetConnectionUnixUser from a table,
so neither a bus nor polkit is needed.
"""

import asyncio
import configparser
import json
import os
import pwd
import re
import shutil
import sys
import time
import types

SOURCE_DIR, WORK_DIR = sys.argv[1:3]

PLACEHOLDERS = {
	"version": "test",
	"config_dir": f"{WORK_DIR}/etc",
	"user_models_dir": f"{WORK_DIR}/models",
	"openvino_cache_dir": f"{WORK_DIR}/cache",
	"dlib_data_dir": f"{WORK_DIR}/dlib",
	"openvino_data_dir": f"{WORK_DIR}/openvino",
	"log_path": f"{WORK_DIR}/log",
	"data_dir": f"{WORK_DIR}/data",
	"gtk_data_dir": f"{WORK_DIR}/gtkdata",
	"authentication_bus": "org.boltgolt.howdy.authentication",
	"authentication_path": "/org/boltgolt/howdy/Authentication",
	"authentication_interface": "org.boltgolt.howdy.Authentication",
	"howdy_authentication_service_name": "howdy-authentication-test.service",
	"popup_path": "/org/boltgolt/howdy/AuthPopup",
	"popup_interface": "org.boltgolt.howdy.AuthPopup",
	"management_bus": "org.boltgolt.howdy.management",
	"management_path": "/org/boltgolt/howdy/Management",
	"management_interface": "org.boltgolt.howdy.Management",
}


def stage():
	"""Copy the package the way it is installed (flat, plus the sub packages)"""
	package = os.path.join(WORK_DIR, "pyhowdy", "howdy")
	shutil.rmtree(os.path.join(WORK_DIR, "pyhowdy"), ignore_errors=True)
	os.makedirs(package)
	flat = [
		os.path.join(SOURCE_DIR, d) for d in ("services", "face", "enroll", "common")
	]
	files = [
		(os.path.join(d, f), os.path.join(package, f))
		for d in flat
		for f in os.listdir(d)
		if f.endswith(".py")
	]
	for sub in ("cli", "recorders", "rubberstamps"):
		os.makedirs(os.path.join(package, sub))
		for f in os.listdir(os.path.join(SOURCE_DIR, sub)):
			if f.endswith(".py"):
				files.append(
					(os.path.join(SOURCE_DIR, sub, f), os.path.join(package, sub, f))
				)

	for source, target in files:
		with open(source) as handle:
			text = handle.read()
		text = re.sub(
			r"@([a-z_]+)@", lambda m: PLACEHOLDERS.get(m.group(1), m.group(0)), text
		)
		with open(target, "w") as handle:
			handle.write(text)
	open(os.path.join(package, "__init__.py"), "a").close()
	return os.path.join(WORK_DIR, "pyhowdy")


sys.path.insert(0, stage())

from dbus_next import DBusError, MessageType  # noqa: E402
from dbus_next.service import ServiceInterface  # noqa: E402

from howdy import howdy_authentication_interface as auth_module  # noqa: E402
from howdy import howdy_management_daemon as management_module  # noqa: E402
from howdy.caller import (  # noqa: E402
	CallerError,
	MissingSenderError,
	is_privileged,
	may_access,
	query_caller_uid,
)
from howdy.cli.auth import check_authorization  # noqa: E402

failed = False


def check(name, got, want):
	global failed
	ok = got == want
	failed = failed or not ok
	print(
		("PASS  " if ok else "FAIL  ")
		+ name
		+ ("" if ok else f": got {got!r}, expected {want!r}")
	)


class Bus:
	"""Answers GetConnectionUnixUser from a table of bus name to uid"""

	def __init__(self, uids=None, error=None):
		self.uids = uids or {}
		self.error = error

	async def call(self, msg):
		if self.error:
			return types.SimpleNamespace(
				message_type=MessageType.ERROR, body=[self.error]
			)
		return types.SimpleNamespace(
			message_type=MessageType.METHOD_RETURN, body=[self.uids[msg.body[0]]]
		)


async def outcome(coroutine):
	"""The result of a call, or the name of its error"""
	try:
		return await coroutine
	except DBusError as exc:
		return exc.type
	except Exception as exc:
		return type(exc).__name__


DAEMON = os.geteuid()
OWNER, OTHER = 4242, 4243
# A uid that is neither root nor the user of the daemon, whoever runs the test
if DAEMON in (OWNER, OTHER):
	OWNER, OTHER = 5252, 5253

print("--- common caller checks")
check("root is privileged", is_privileged(0), True)
check(
	"the daemon user is not privileged by default", is_privileged(DAEMON), DAEMON == 0
)
check("the daemon user is privileged if trusted", is_privileged(DAEMON, True), True)
check("somebody else is not privileged", is_privileged(OTHER, True), False)
check("the owner may access", may_access(OWNER, OWNER), True)
check("somebody else may not access", may_access(OTHER, OWNER), False)
check("root may access", may_access(0, OWNER), True)
check("the daemon user may access if trusted", may_access(DAEMON, OWNER, True), True)
check(
	"the daemon user may not access by default", may_access(DAEMON, OWNER), DAEMON == 0
)


async def common():
	check("uid from the bus", await query_caller_uid(Bus({":1.1": 1234}), ":1.1"), 1234)
	for sender in (None, ""):
		try:
			await query_caller_uid(Bus(), sender)
			result = "no error"
		except MissingSenderError:
			result = "missing sender"
		check(f"sender {sender!r}", result, "missing sender")
	try:
		await query_caller_uid(Bus(error="gone"), ":1.1")
		result = "no error"
	except MissingSenderError:
		result = "missing sender"
	except CallerError:
		result = "caller error"
	check("error of the bus", result, "caller error")


asyncio.run(common())

print("--- authentication daemon")
INTERFACE = object.__new__(auth_module.AuthenticationInterface)
ServiceInterface.__init__(INTERFACE, "org.boltgolt.howdy.Authentication")
METHODS = {m.name: m.fn for m in ServiceInterface._get_methods(INTERFACE)}


def new_interface(uids):
	import threading

	interface = INTERFACE
	interface.bus = Bus(uids)
	interface.lock = threading.Lock()
	interface.requests = {}
	interface.cooldown_until = {}
	interface.busy_callers = set()
	return interface


def request(status=auth_module.HowdyStatus.Running):
	import threading

	return {
		"owner": OWNER,
		"status": status,
		"cancel": threading.Event(),
		"timeout": auth_module.Timeout(0),
	}


UIDS = {":owner": OWNER, ":other": OTHER, ":root": 0, ":daemon": DAEMON}


async def as_caller(name, method, *args):
	auth_module.current_sender.set(name)
	return await outcome(METHODS[method](INTERFACE, *args))


async def authentication():
	new_interface(UIDS)

	INTERFACE.requests["r1"] = request()
	check(
		"owner sees the result",
		await as_caller(":owner", "GetResult", "r1"),
		str(auth_module.HowdyStatus.Running),
	)
	check(
		"somebody else does not see it",
		await as_caller(":other", "GetResult", "r1"),
		"org.howdy.Authentication.UnknownRequest",
	)
	check(
		"root sees the result",
		await as_caller(":root", "GetResult", "r1"),
		str(auth_module.HowdyStatus.Running),
	)
	check(
		"the daemon user sees the result",
		await as_caller(":daemon", "GetResult", "r1"),
		str(auth_module.HowdyStatus.Running),
	)

	check(
		"somebody else can not cancel",
		await as_caller(":other", "CancelAuthentication", "r1"),
		False,
	)
	check(
		"the request keeps running",
		INTERFACE.requests["r1"]["status"],
		auth_module.HowdyStatus.Running,
	)
	check(
		"owner cancels", await as_caller(":owner", "CancelAuthentication", "r1"), True
	)
	check(
		"the request is cancelling",
		INTERFACE.requests["r1"]["status"],
		auth_module.HowdyStatus.Cancelling,
	)

	check(
		"somebody else can not clear",
		await as_caller(":other", "ClearAuthentication", "r1"),
		False,
	)
	check("the request still exists", "r1" in INTERFACE.requests, True)
	check("owner clears", await as_caller(":owner", "ClearAuthentication", "r1"), True)
	check("the request is gone", "r1" in INTERFACE.requests, False)
	INTERFACE.requests["r2"] = request()
	check("root clears", await as_caller(":root", "ClearAuthentication", "r2"), True)
	check(
		"unknown request is not cleared",
		await as_caller(":root", "ClearAuthentication", "r2"),
		False,
	)

	INTERFACE.requests["r3"] = request()
	check("the cleanup clears every request", INTERFACE.clear("r3"), True)

	check(
		"a call without a sender is not authorized",
		await as_caller(None, "GetResult", "r1"),
		"org.howdy.Authentication.NotAuthorized",
	)
	INTERFACE.bus = Bus(error="gone")
	check(
		"a caller that is not known is not authorized",
		await as_caller(":owner", "GetResult", "r1"),
		"org.howdy.Authentication.NotAuthorized",
	)
	INTERFACE.bus = Bus(UIDS)

	# A user without a face model is answered at once, also while a scan runs, and is not made to wait
	INTERFACE.authenticator = types.SimpleNamespace(
		has_face_model=lambda username: False, start_cooldown=0
	)
	INTERFACE.active_request = "running"
	started = await as_caller(":daemon", "StartAuthentication", "root", [])
	check(
		"no model: the request is accepted while a scan runs",
		isinstance(started, str) and len(started) > 0,
		True,
	)
	check(
		"no model: the result is known at once",
		await as_caller(":daemon", "GetResult", started),
		str(auth_module.HowdyStatus.NoEnrolledFaces),
	)
	check(
		"no model: the scan that runs is not touched",
		INTERFACE.active_request,
		"running",
	)
	INTERFACE.authenticator = types.SimpleNamespace(
		has_face_model=lambda username: True, start_cooldown=0
	)
	check(
		"with a model the running scan makes the caller wait",
		await as_caller(":daemon", "StartAuthentication", "root", []),
		"org.howdy.Authentication.Busy",
	)
	INTERFACE.active_request = None

	# Nobody can ask again and again, the cooldown applies to a user without a model, too
	nobody = pwd.getpwnam("nobody").pw_uid
	INTERFACE.bus = Bus({":nobody": nobody})
	INTERFACE.authenticator = types.SimpleNamespace(
		has_face_model=lambda username: False, start_cooldown=60
	)
	first = await as_caller(":nobody", "StartAuthentication", "nobody", [])
	check("cooldown: the first request is accepted", isinstance(first, str), True)
	check(
		"cooldown: the next one has to wait",
		await as_caller(":nobody", "StartAuthentication", "nobody", []),
		"org.howdy.Authentication.Busy",
	)

	# A caller without privileges that was told that an authentication is running is told so again without
	# its model being read once more, until that one is over
	INTERFACE.bus = Bus({**UIDS, ":nobody": nobody})
	reads = []
	model = [True]

	def has_face_model(username):
		reads.append(username)
		return model[0]

	INTERFACE.authenticator = types.SimpleNamespace(
		has_face_model=has_face_model,
		start_cooldown=60,
		authenticate=lambda *args: auth_module.HowdyStatus.Success,
	)
	check(
		"in its cooldown: refused",
		await as_caller(":nobody", "StartAuthentication", "nobody", []),
		"org.howdy.Authentication.Busy",
	)
	check("in its cooldown: the model is not read", len(reads), 0)
	INTERFACE.cooldown_until.clear()
	INTERFACE.authenticator.start_cooldown = 0
	INTERFACE.active_request = "running"
	check(
		"while a scan runs: refused",
		await as_caller(":nobody", "StartAuthentication", "nobody", []),
		"org.howdy.Authentication.Busy",
	)
	check("while a scan runs: the model was read", len(reads), 1)
	# Without the model the answer would be that there is none, if the daemon looked for it again
	model[0] = False
	check(
		"asked again: refused",
		await as_caller(":nobody", "StartAuthentication", "nobody", []),
		"org.howdy.Authentication.Busy",
	)
	check("asked again: the model is not read again", len(reads), 1)
	model[0] = True
	for _ in range(2):
		await as_caller(":daemon", "StartAuthentication", "root", [])
	check("a caller with privileges is looked at every time", len(reads), 3)
	check("and is not remembered", INTERFACE.busy_callers, {nobody})

	# The next authentication that starts forgets who was refused, and so does its end
	workers = []
	INTERFACE.executor = types.SimpleNamespace(submit=workers.append)
	INTERFACE._clear_later = lambda request_id: None
	INTERFACE.active_request = None
	started = await as_caller(":daemon", "StartAuthentication", "root", [])
	check("the next scan starts", INTERFACE.active_request, started)
	check("a new scan forgets who was refused", INTERFACE.busy_callers, set())
	await as_caller(":nobody", "StartAuthentication", "nobody", [])
	check("refused during it: remembered", INTERFACE.busy_callers, {nobody})
	workers[0]()
	check("the scan is over", INTERFACE.active_request, None)
	check("its end forgets who was refused", INTERFACE.busy_callers, set())
	model[0] = False
	again = await as_caller(":nobody", "StartAuthentication", "nobody", [])
	check(
		"then the caller is looked at again: no model",
		await as_caller(":nobody", "GetResult", again),
		str(auth_module.HowdyStatus.NoEnrolledFaces),
	)
	del INTERFACE._clear_later
	INTERFACE.bus = Bus(UIDS)

	# Only the own user can be authenticated, everybody else is refused before anything starts
	check(
		"an ordinary user can not authenticate somebody else",
		await as_caller(":other", "StartAuthentication", "root", []),
		"org.howdy.Authentication.NotAuthorized",
	)
	check(
		"unknown user",
		await as_caller(":owner", "StartAuthentication", "no-such-user-howdy", []),
		"org.howdy.Authentication.InvalidUser",
	)


asyncio.run(authentication())

print("--- management daemon")


async def management():
	own = pwd.getpwuid(DAEMON).pw_name
	calls = []

	async def polkit(**kwargs):
		calls.append(kwargs)

	original = management_module.check_authorization
	management_module.check_authorization = polkit
	try:
		interface = management_module.ManagementInterface(Bus({":1.9": DAEMON}))
		message = types.SimpleNamespace(sender=":1.9", body=[])
		check("the caller is the user", await interface.get_caller(message, 0), own)
		check("the uid is passed on", calls[-1]["caller_uid"], DAEMON)
		check(
			"a user name is used",
			await interface.get_caller(
				types.SimpleNamespace(sender=":1.9", body=["root"]), 0
			),
			"root",
		)

		for name, bus, sender, want in (
			("no sender", Bus(), None, "NotAuthorized"),
			("error of the bus", Bus(error="gone"), ":1.9", "CallerError"),
		):
			try:
				await management_module.ManagementInterface(bus).get_caller(
					types.SimpleNamespace(sender=sender, body=[]), 0
				)
				result = "no error"
			except management_module.NotAuthorized:
				result = "NotAuthorized"
			except CallerError:
				result = "CallerError"
			check(name, result, want)
	finally:
		management_module.check_authorization = original

	# The polkit check of the cli package: only root and the user may manage the models of a user
	for name, caller, target, want in (
		("root manages the models of a user", 0, own, "asks polkit"),
		("a user manages the own models", DAEMON, own, "asks polkit"),
		("a user does not manage the models of root", OTHER, "root", "PermissionError"),
		(
			"a user does not manage the models of another user",
			DAEMON,
			"root",
			"PermissionError" if DAEMON != 0 else "asks polkit",
		),
		("unknown user", 0, "no-such-user-howdy", "ValueError"),
	):
		try:
			await check_authorization(
				bus=PolkitBus(), bus_name=":1.9", caller_uid=caller, target_user=target
			)
			result = "asks polkit"
		except PermissionError:
			result = "PermissionError"
		except ValueError:
			result = "ValueError"
		check(name, result, want)


class PolkitBus:
	async def call(self, msg):
		return types.SimpleNamespace(
			message_type=MessageType.METHOD_RETURN,
			body=[[True, False, {}]],
			signature="(bba{ss})",
		)


asyncio.run(management())

print("--- management daemon, comparing a face")
from dbus_next import Message  # noqa: E402

from howdy import paths_factory  # noqa: E402
from howdy.file_util import compare_face  # noqa: E402

NEAR = {"label": "near", "data": [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0]]}
FAR = {"label": "far", "data": [[5.0, 5.0, 5.0, 5.0]]}
OTHER_SIZE = {"label": "other backend", "data": [[1.0, 2.0]]}
MODELS = [NEAR, FAR, OTHER_SIZE]


def best(metric, face):
	try:
		return compare_face(MODELS, metric, face)
	except ValueError as exc:
		return str(exc)


check("the closest encoding wins", best("euclidean", [0.0, 0.9, 0.0, 0.0])[1], "near")
check(
	"and its distance is given",
	round(best("euclidean", [0.0, 0.9, 0.0, 0.0])[0], 9),
	0.1,
)
check("the largest dot product wins", best("dot", [0.0, 0.5, 0.0, 0.0]), (2.5, "far"))
check(
	"the dot product adds all values", best("dot", [1.0, 1.0, 1.0, 1.0]), (20.0, "far")
)
check("models of another size are skipped", best("euclidean", [1.0, 2.0, 3.0]), None)
check("an unknown metric is refused", best("cosine", [1.0]), "Unknown metric cosine")
check("an empty face is refused", best("euclidean", []), "Invalid face encoding")


class Recorder(Bus):
	"""A bus that keeps what the daemon sends"""

	def __init__(self, uids):
		super().__init__(uids)
		self.sent = []

	def send(self, message):
		self.sent.append(message)


async def comparing():
	# Only root compares faces: the models are the ones of root, the caller without a user name is root
	own = "root"
	models_dir = os.path.join(WORK_DIR, "models")
	shutil.rmtree(models_dir, ignore_errors=True)
	os.makedirs(models_dir, mode=0o700)
	os.chmod(models_dir, 0o700)

	polkit_calls = []
	polkit_refuses = False

	async def polkit(**kwargs):
		polkit_calls.append(kwargs["bus_name"])
		if polkit_refuses:
			raise PermissionError("Polkit authorization denied")

	original = management_module.check_authorization
	management_module.check_authorization = polkit

	# :1.9 is root, :1.11 another user, :1.12 the user of the daemon (not root, unless the test runs as root)
	bus = Recorder({":1.9": 0, ":1.11": OTHER, ":1.12": DAEMON})
	interface = management_module.ManagementInterface(bus)

	async def compare(signature, body, sender=":1.9"):
		"""The reply of Compare: its body, or the name of its error"""
		bus.sent.clear()
		call = Message(
			destination="org.example.d", path="/p", interface="org.example.I", member="Compare",
			signature=signature, body=body, sender=sender, serial=1,
		)  # fmt: skip
		await interface.compare(call)
		reply = bus.sent[0]
		if reply.message_type == MessageType.ERROR:
			return reply.error_name
		return reply.body

	try:
		check(
			"a user without models has no match",
			await compare("sad", ["euclidean", [1.0]]),
			[False, 0.0, ""],
		)

		path = paths_factory.user_model_path(own)
		with open(path, "w") as handle:
			json.dump(MODELS, handle)
		os.chmod(path, 0o600)

		found = await compare("sad", ["euclidean", [0.0, 0.9, 0.0, 0.0]])
		check(
			"the face is compared with the own models",
			(found[0], found[2]),
			(True, "near"),
		)
		found = await compare("ssad", [own, "dot", [1.0, 1.0, 1.0, 1.0]])
		check("with the user name", (found[0], found[1], found[2]), (True, 20.0, "far"))
		check(
			"the daemon skips models of another size",
			(await compare("sad", ["euclidean", [1.0, 2.0, 3.0]]))[0],
			False,
		)
		check(
			"an unknown metric is an error",
			await compare("sad", ["cosine", [1.0]]),
			COMPARE_ERROR,
		)
		# Only root is let in, without polkit
		check("polkit is not asked for compare", polkit_calls, [])
		check(
			"root compares for another user",
			await compare("ssad", ["nobody", "euclidean", [1.0]]),
			[False, 0.0, ""],
		)
		check(
			"an unknown user is an error",
			await compare("ssad", ["no-such-user-howdy", "euclidean", [1.0]]),
			COMPARE_ERROR,
		)
		# Nobody else is trusted, also not the user the daemon runs as: the daemon has no mode for the tests
		if DAEMON != 0:
			check(
				"the user of the daemon is refused",
				await compare("sad", ["euclidean", [1.0]], sender=":1.12"),
				NOT_AUTHORIZED_ERROR,
			)
		check(
			"another user is refused",
			await compare("sad", ["euclidean", [1.0]], sender=":1.11"),
			NOT_AUTHORIZED_ERROR,
		)
		check(
			"and with the user name",
			await compare("ssad", [own, "euclidean", [1.0]], sender=":1.11"),
			NOT_AUTHORIZED_ERROR,
		)
		check("and polkit is not asked for it", polkit_calls, [])

		# A refusal of polkit does not matter
		polkit_refuses = True
		check(
			"a refusal by polkit does not matter",
			(await compare("sad", ["euclidean", [0.0, 0.9, 0.0, 0.0]]))[2],
			"near",
		)
		polkit_refuses = False

		check(
			"no sender",
			await compare("sad", ["euclidean", [1.0]], sender=None),
			NOT_AUTHORIZED_ERROR,
		)
		check(
			"KeepAlive is gone",
			hasattr(interface, "keep_alive") or "KeepAlive" in interface.methods,
			False,
		)
	finally:
		management_module.check_authorization = original


COMPARE_ERROR = management_module.COMPARE_ERROR
NOT_AUTHORIZED_ERROR = management_module.NOT_AUTHORIZED_ERROR
asyncio.run(comparing())

print("--- the D-Bus policy")
from xml.etree import ElementTree

POLICY = os.path.join(
	SOURCE_DIR, "..", "..", "assets", "bin", "org.boltgolt.howdy.conf.in"
)
with open(POLICY) as handle:
	policy_text = re.sub(
		r"@([a-z_]+)@",
		lambda m: PLACEHOLDERS.get(m.group(1), m.group(0)),
		handle.read(),
	)
policies = ElementTree.fromstring(policy_text).findall("policy")


def members(policy):
	"""The methods of the management interface that a policy lets through"""
	return {
		rule.get("send_member")
		for rule in policy.findall("allow")
		if rule.get("send_destination") == PLACEHOLDERS["management_bus"]
		and rule.get("send_member")
	}


root_policy = next(p for p in policies if p.get("user") == "root")
default_policy = next(p for p in policies if p.get("context") == "default")
check("root may call Compare", "Compare" in members(root_policy), True)
check("a normal user may not call Compare", "Compare" in members(default_policy), False)
check(
	"and there is no KeepAlive any more",
	"KeepAlive" in members(root_policy) | members(default_policy),
	False,
)
check(
	"the other methods stay for everybody",
	{"Add", "List", "Remove", "Rename", "Clear"} <= members(default_policy),
	True,
)

print("--- management daemon, removing and renaming models")


def saved_models(user):
	with open(paths_factory.user_model_path(user)) as handle:
		return json.load(handle)


async def managing():
	own = pwd.getpwuid(DAEMON).pw_name
	models_dir = os.path.join(WORK_DIR, "models")
	shutil.rmtree(models_dir, ignore_errors=True)
	os.makedirs(models_dir, mode=0o700)
	os.chmod(models_dir, 0o700)

	async def polkit(**kwargs):
		pass

	original = management_module.check_authorization
	management_module.check_authorization = polkit

	bus = Recorder({":1.9": DAEMON})
	interface = management_module.ManagementInterface(bus)

	async def call(member, signature, body):
		"""The reply of a method: its body, or the name of its error and its text"""
		bus.sent.clear()
		message = Message(
			destination="org.example.d", path="/p", interface="org.example.I", member=member,
			signature=signature, body=body, sender=":1.9", serial=1,
		)  # fmt: skip
		await getattr(interface, member.lower())(message)
		reply = bus.sent[0]
		if reply.message_type == MessageType.ERROR:
			return reply.error_name, reply.body[0]
		return reply.body

	async def models():
		"""The (id, label) of the models of the user"""
		listed = json.loads((await call("List", "", []))[0])
		return [(model["id"], model["label"]) for model in listed]

	def model(label):
		return json.dumps({"time": 1, "label": label, "data": [[0.1, 0.2]]})

	try:
		for label in ("first", "second", "third"):
			await call("Add", "s", [model(label)])
		check(
			"models get the next ids",
			await models(),
			[(0, "first"), (1, "second"), (2, "third")],
		)

		check(
			"rename",
			await call("Rename", "xsxs", [1, "second", 1, "Living room"]),
			[True],
		)
		check(
			"only the label changes",
			await models(),
			[(0, "first"), (1, "Living room"), (2, "third")],
		)
		check(
			"rename with a user",
			await call("Rename", "sxsxs", [own, 2, "third", 1, "Office"]),
			[True],
		)
		check(
			"rename with a user, the label",
			(await models())[2],
			(2, "Office"),
		)
		check(
			"the encodings are kept",
			saved_models(own)[1]["data"],
			[[0.1, 0.2]],
		)

		error, text = await call("Rename", "xsxs", [9, "x", 1, "y"])
		check("rename unknown id, error", error, management_module.RENAME_ERROR)
		check("rename unknown id, text", text, f"No model with ID 9 exists for {own}")
		error, text = await call("Rename", "xsxs", [1, "Living room", 1, "x" * 300])
		check("rename with a long label, error", error, management_module.RENAME_ERROR)
		check("rename with a long label, text", text, "Model label is too long")
		# The caller saw the models before another one was removed: the ids moved up
		stale = "Model {} has changed since the list was loaded, please reload the list"
		check(
			"rename with an old list",
			await call("Rename", "xsxs", [1, "second", 1, "x"]),
			(management_module.RENAME_ERROR, stale.format(1)),
		)
		check(
			"rename with another time",
			await call("Rename", "xsxs", [1, "Living room", 5, "x"]),
			(management_module.RENAME_ERROR, stale.format(1)),
		)
		check(
			"remove with an old list",
			await call("Remove", "xsx", [0, "wrong", 1]),
			(management_module.REMOVE_ERROR, stale.format(0)),
		)
		check(
			"remove with another time",
			await call("Remove", "xsx", [0, "first", 5]),
			(management_module.REMOVE_ERROR, stale.format(0)),
		)
		for name, label, text in (
			("an empty label", "", "Model label is empty"),
			("a control character", "a\x1bb", "Model label has invalid characters"),
			("a C1 control character", "a\x9bb", "Model label has invalid characters"),
			("a line break", "a\nb", "Model label has invalid characters"),
		):
			check(
				f"rename with {name}",
				await call("Rename", "xsxs", [1, "Living room", 1, label]),
				(management_module.RENAME_ERROR, text),
			)
			check(
				f"add with {name}",
				await call("Add", "s", [model(label)]),
				(management_module.ADD_ERROR, text),
			)
		check(
			"nothing changed by the failures",
			await models(),
			[(0, "first"), (1, "Living room"), (2, "Office")],
		)

		check("remove the first", await call("Remove", "xsx", [0, "first", 1]), [True])
		check(
			"the models behind it move up",
			await models(),
			[(0, "Living room"), (1, "Office")],
		)
		error, text = await call("Remove", "xsx", [5, "x", 1])
		check("remove unknown id, error", error, management_module.REMOVE_ERROR)
		check("remove unknown id, text", text, f"No model with ID 5 exists for {own}")
		check(
			"nothing changed by the failures, remove",
			await models(),
			[(0, "Living room"), (1, "Office")],
		)

		check(
			"remove the last one of the list",
			await call("Remove", "xsx", [1, "Office", 1]),
			[True],
		)
		check("the ones before it stay", await models(), [(0, "Living room")])
		check(
			"remove the last model",
			await call("Remove", "sxsx", [own, 0, "Living room", 1]),
			[True],
		)
		check(
			"the file is gone with the last model",
			os.path.exists(paths_factory.user_model_path(own)),
			False,
		)
		error, text = await call("Rename", "xsxs", [0, "x", 1, "y"])
		check("rename without models, error", error, management_module.RENAME_ERROR)
		check(
			"rename without models, text", text, f"No face model known for user {own}"
		)
		check(
			"a new model starts at 0 again",
			await call("Add", "s", [model("new")]),
			[True],
		)
		check("the id", await models(), [(0, "new")])

		# The same limits as the C++ daemon has
		good = {"time": 1, "label": "x", "data": [[0.1]]}
		for name, change, text in (
			("no time", {"time": None}, "Model has no valid time"),
			(
				"a time that is no number",
				{"time": float("nan")},
				"Model has no valid time",
			),
			("a time of 1e300", {"time": 1e300}, "Model has no valid time"),
			("a time of 2^63", {"time": 2**63}, "Model has no valid time"),
			("no data", {"data": None}, "Model has no valid data"),
			("text as encoding", {"data": [["a"]]}, "Model has no valid data"),
			("an encoding too long", {"data": [[1] * 513]}, "Model has no valid data"),
			(
				"too many encodings",
				{"data": [[1]] * 65},
				"Model has too many encodings",
			),
		):
			check(
				f"add with {name}",
				await call("Add", "s", [json.dumps({**good, **change})]),
				(management_module.ADD_ERROR, text),
			)
		await call("Add", "s", [json.dumps({**good, "extra": "unlimited"})])
		with open(paths_factory.user_model_path(own)) as handle:
			stored = json.load(handle)
		check(
			"other fields of a model are not stored",
			sorted(stored[-1]),
			["data", "id", "label", "time"],
		)
		for _ in range(70):
			last = await call("Add", "s", [json.dumps(good)])
		check(
			"too many models",
			last,
			(
				management_module.ADD_ERROR,
				"Too many face models, please remove one first",
			),
		)
		check("no more are stored", len(await models()), 64)
	finally:
		management_module.check_authorization = original


asyncio.run(managing())

print("--- the timeout a caller can ask for")


def asked_timeout(configured, value, limit):
	daemon = types.SimpleNamespace(auth_popup=False, timeout=configured)
	parse = auth_module.HowdyAuthenticator._parse_args
	return parse(daemon, [f"timeout={value}"], limit).timeout


check("a privileged caller: as asked", asked_timeout(4, 600, False), 600)
check("a privileged caller: no limit", asked_timeout(4, -1, False), sys.maxsize)
check("without privileges: at most 60s", asked_timeout(4, 600, True), 60)
check("without privileges: no limit is 60s", asked_timeout(4, -1, True), 60)
check("without privileges: shorter is fine", asked_timeout(4, 10, True), 10)
check("without privileges: the config allows more", asked_timeout(120, -1, True), 120)
check("without privileges: no limit in the config", asked_timeout(-1, 600, True), 600)

print("--- a rubber stamp that can not run")
from howdy import rubberstamps  # noqa: E402

rules = configparser.ConfigParser()
rules.read_dict({"rubberstamps": {"stamp_rules": "nod 1s failsafe"}})
# Without a camera the stamp fails with an error
broken = dict.fromkeys(("video_capture", "face_detector", "pose_predictor", "clahe"))
check(
	"it does not let the authentication pass",
	rubberstamps.execute(rules, lambda **kwargs: None, broken),
	False,
)
for name, rule in (
	("a rule that can not be read", "nod faildeadly"),
	("a stamp that is not installed", "nodd 5s faildeadly"),
):
	rules.read_dict({"rubberstamps": {"stamp_rules": rule}})
	check(
		f"{name} denies",
		rubberstamps.execute(rules, lambda **kwargs: None, broken),
		False,
	)

# All rules are read first, no stamp runs when a later line denies anyway
from howdy.rubberstamps.nod import nod  # noqa: E402

ran = []
original_run = nod.run
nod.run = lambda self: ran.append(self.options["min_distance"]) or True
for name, stamp_rules, expected in (
	("two valid rules pass", "nod 1s failsafe\nnod 1s failsafe min_distance=9", [6, 9]),
	("a later stamp that is not installed", "nod 1s failsafe\nnodd 1s failsafe", []),
	("an argument without =", "nod 1s failsafe min_distance", []),
	("an option the stamp does not have", "nod 1s failsafe min_directons=4", []),
	("the timeout as an argument", "nod 1s failsafe timeout=5", []),
	("a comment behind a rule", "nod 1s failsafe # nod", []),
	("a value that is no number", "nod 1s failsafe min_distance=far", []),
	("a timeout that is no number", "nod s failsafe", []),
):
	rules.read_dict({"rubberstamps": {"stamp_rules": stamp_rules}})
	ran.clear()
	passed = rubberstamps.execute(rules, lambda **kwargs: None, broken)
	check(name, (passed, ran), (bool(expected), expected))
nod.run = original_run

print("--- a rubber stamp looks after the popup and the one who waits")
import threading  # noqa: E402

from howdy import howdy_popup_interface as popup_module  # noqa: E402

alive = []
stamp = rubberstamps.RubberStamp()
stamp.keep_alive = lambda: alive.append(True)
stamp.sleep(0.35)
check("it keeps the popup alive while it sleeps", len(alive) >= 4, True)


def stops(**attributes):
	"""Whether a sleep of 5 seconds ends early with these attributes of the stamp"""
	instance = rubberstamps.RubberStamp()
	for name, value in attributes.items():
		setattr(instance, name, value)
	started = time.monotonic()
	try:
		instance.sleep(5)
	except rubberstamps.StampCancelled:
		return time.monotonic() - started < 1
	return False


check("it stops when nobody waits any more", stops(abandoned=lambda: True), True)
gone_at = time.monotonic() + 0.2
check(
	"also in the middle of a sleep",
	stops(abandoned=lambda: time.monotonic() > gone_at, cancel_event=threading.Event()),
	True,
)

popup = popup_module.UIInterface()
sent = []
popup.UpdateMessage = lambda uid, message: sent.append(message)
popup.keep_alive(1)
check("no sign of life without a popup", sent, [])
# Without a bus the signal goes nowhere, what the popup shows is remembered
popup.Show(1, "hello")
popup.keep_alive(1)
check("none right after a signal", sent, [])
time.sleep(popup_module.POPUP_KEEPALIVE + 0.05)
popup.keep_alive(1)
popup.keep_alive(1)
check("after a pause the text once more", sent, ["hello"])
popup.update_message(1, "nod")
time.sleep(popup_module.POPUP_KEEPALIVE + 0.05)
popup.keep_alive(1)
check("with the text it shows now", sent, ["hello", "nod", "nod"])
popup.Close(1)
time.sleep(popup_module.POPUP_KEEPALIVE + 0.05)
popup.keep_alive(1)
check("none after the popup was closed", len(sent), 3)

print("--- ffmpeg that does not deliver")
import subprocess  # noqa: E402


class FfmpegStream:
	"""What the reader uses of the package ffmpeg-python, which is not needed for this"""

	def input(self, *args, **kwargs):
		return self

	def output(self, *args, **kwargs):
		return self

	def run_async(self, **kwargs):
		pipe = subprocess.PIPE
		return subprocess.Popen(["ffmpeg"], stdout=pipe, stderr=pipe)


# Neither is pyv4l2, both programs are stand-ins
sys.modules["ffmpeg"] = FfmpegStream()
sys.modules["pyv4l2"] = types.ModuleType("pyv4l2")
sys.modules["pyv4l2.frame"] = types.SimpleNamespace(Frame=None)
from howdy.recorders import ffmpeg_reader, pyv4l2_reader  # noqa: E402
from howdy.recorders.video_capture import NoDeviceException  # noqa: E402


def no_device(call):
	"""What the call says when it gives the camera up, else what it did"""
	try:
		return call()
	except NoDeviceException:
		return "no device"
	except Exception as exc:
		return type(exc).__name__


def stand_in(script):
	directory = os.path.join(WORK_DIR, "stand-in")
	os.makedirs(directory, exist_ok=True)
	for name in ("ffmpeg", "ffprobe"):
		path = os.path.join(directory, name)
		with open(path, "w") as handle:
			handle.write("#!/bin/sh\n" + script + "\n")
		os.chmod(path, 0o755)
	os.environ["PATH"] = directory + ":" + original_path


def reader(size=0):
	result = ffmpeg_reader.ffmpeg_reader("/dev/null", "v4l2")
	result.width = result.height = size
	return result


def by_ioctl(device="/dev/null"):
	"""The ioctl fails on a device that is no camera"""
	return pyv4l2_reader.pyv4l2_reader(device, "v4l2")


original_path = os.environ["PATH"]
ffmpeg_reader.PROCESS_TIMEOUT = 1
stand_in("exec sleep 30")
check("no resolution in time", no_device(reader().probe), "no device")
check("no frames in time", no_device(lambda: reader(2).record(1)), "no device")
check("pyv4l2: no resolution in time", no_device(by_ioctl), "no device")
stand_in("exit 1")
check("no resolution", no_device(reader().probe), "no device")
check("ffmpeg fails", no_device(lambda: reader(2).record(1)), "no device")
check("pyv4l2: no resolution", no_device(by_ioctl), "no device")
check(
	"pyv4l2: no device at all",
	no_device(lambda: by_ioctl("/nonexistent")),
	"no device",
)
stand_in("exit 0")
check("no frame at all", no_device(lambda: reader(2).record(1)), "no device")
stand_in("echo 640x480")
check("pyv4l2: the resolution of ffprobe", by_ioctl().width, 640)


def pyv4l2_frame(size):
	"""A frame of a 640x480 camera that delivers this many bytes"""
	pyv4l2_reader.Frame = lambda _: types.SimpleNamespace(get_frame=lambda: bytes(size))
	camera = by_ioctl()
	# What the config asks for does not change what the device delivers
	camera.width = 320
	ok, frame = camera.read()
	return ok, frame.shape


check(
	"pyv4l2: a frame of the size of the camera",
	pyv4l2_frame(640 * 480),
	(1, (480, 640, 3)),
)
check(
	"pyv4l2: a frame of another size",
	no_device(lambda: pyv4l2_frame(100)),
	"no device",
)
os.environ["PATH"] = original_path

# Three frames of 2x2 pixels, ten are asked for
stand_in("head -c 36 /dev/zero")
short = reader(2)
check(
	"ffmpeg: the frames that came",
	[no_device(lambda: short.read()[0]) for _ in range(4)],
	[1, 1, 1, "no device"],
)
os.environ["PATH"] = original_path

print("--- the cache directory of OpenVINO")
# Neither openvino nor dlib is needed for this
original_dlib = sys.modules.get("dlib")
sys.modules["dlib"] = types.ModuleType("dlib")
sys.modules["openvino"] = types.SimpleNamespace(
	Core=lambda: types.SimpleNamespace(set_property=lambda _: None)
)
from howdy import openvino_face  # noqa: E402

cache = os.path.join(WORK_DIR, "ov-home", "openvino")
shutil.rmtree(os.path.dirname(cache), ignore_errors=True)
openvino_face._cache_dir = lambda: cache
previous_umask = os.umask(0o022)
openvino_face._get_core()
os.umask(previous_umask)
for name in ("", "cl_cache", "intel_ocl"):
	check(
		f"only for its owner: openvino/{name}",
		oct(os.stat(os.path.join(cache, name)).st_mode & 0o777),
		oct(0o700),
	)
del sys.modules["openvino"], sys.modules["dlib"]
if original_dlib is not None:
	sys.modules["dlib"] = original_dlib

print("--- the certainty on a snapshot")
from howdy import snapshot  # noqa: E402
from howdy.recorders import video_capture as capture_module  # noqa: E402

snapshot_config = os.path.join(WORK_DIR, "snapshot.ini")
with open(snapshot_config, "w") as handle:
	handle.write("[face_authentication]\ncertainty = 88\n[video]\n")
written = []
originals = (
	capture_module.VideoCapture,
	snapshot.generate,
	paths_factory.config_file_path,
)
capture_module.VideoCapture = lambda _: types.SimpleNamespace(
	read_frame=lambda: (None, None)
)
snapshot.generate = lambda frames, lines: written.extend(lines) or "file"
paths_factory.config_file_path = lambda: snapshot_config
from howdy.cli import snap  # noqa: E402,F401

capture_module.VideoCapture, snapshot.generate, paths_factory.config_file_path = (
	originals
)
check("it is the one of the config", written[-1].endswith("88.0"), True)

print("--- howdy add and clear reach the management daemon")
import builtins  # noqa: E402

from howdy import openvino_face  # noqa: E402
from howdy.cli import add, clear  # noqa: E402

add_config = os.path.join(WORK_DIR, "add.ini")
with open(add_config, "w") as handle:
	handle.write("[face_authentication]\nuse_openvino = true\n[video]\n")
sent = []
cleared = []


class ManagementBus:
	"""A management daemon that has no model yet and keeps what is added"""

	def __init__(self, **kwargs):
		pass

	async def connect(self):
		return self

	async def call(self, message):
		if message.member == "Add":
			sent.append(json.loads(message.body[-1]))
		if message.member == "Clear":
			cleared.append(message.body)
		body = ["[]"] if message.member == "List" else [True]
		return types.SimpleNamespace(message_type=None, body=body)

	def disconnect(self):
		pass


async def authorized(**kwargs):
	pass


# Only the saving is tested: no camera, no models
patched = (
	(add, "MessageBus", ManagementBus),
	(add, "check_authorization", authorized),
	(add, "VideoCapture", lambda _: types.SimpleNamespace(release=lambda: None)),
	(add.enroll_capture, "capture_guided_samples", lambda **kwargs: [[0.5, 0.25]]),
	(add.paths_factory, "config_file_path", lambda: add_config),
	(openvino_face, "FaceDetector", lambda **kwargs: None),
	(openvino_face, "FaceEncoder", lambda **kwargs: None),
	(clear, "MessageBus", ManagementBus),
	(clear, "check_authorization", authorized),
)
originals = [(owner, name, getattr(owner, name)) for owner, name, _ in patched]
for owner, name, value in patched:
	setattr(owner, name, value)
builtins.howdy_args = types.SimpleNamespace(
	user="someone", gui=False, plain=True, y=True, arguments=["laptop"]
)


def exit_code(command):
	try:
		command()
	except SystemExit as exc:
		return exc.code
	return 0


before = int(time.time())
add_code = exit_code(add.run)
clear_code = exit_code(clear.run)
for owner, name, value in originals:
	setattr(owner, name, value)
check("howdy add ends well", add_code, 0)
check(
	"the model arrives",
	[(model["label"], model["data"]) for model in sent],
	[("laptop", [[0.5, 0.25]])],
)
check(
	"with the date of today",
	[before <= model["time"] <= int(time.time()) for model in sent],
	[True],
)
check(
	"the management daemon takes it",
	[management_module.validate_new_model(model) for model in sent],
	sent,
)
check("howdy clear ends well", clear_code, 0)
check("the models of the user are cleared", cleared, [["someone"]])

released = []


def cancelled(**kwargs):
	raise add.enroll_helper.EnrollCancelled()


def still_reading(**kwargs):
	kwargs["video_capture"].still_reading = True
	raise add.enroll_helper.EnrollCancelled()


for name, capture, want in (
	("a cancel", cancelled, (3, [True])),
	("a cancel while the camera does not answer", still_reading, (3, [])),
):
	for owner, attribute, value in patched:
		setattr(owner, attribute, value)
	add.VideoCapture = lambda _: types.SimpleNamespace(
		release=lambda: released.append(True)
	)
	add.enroll_capture.capture_guided_samples = capture
	released.clear()
	code = exit_code(add.run)
	for owner, attribute, value in originals:
		setattr(owner, attribute, value)
	check(f"howdy add, {name}: exit status and release", (code, released), want)

print("--- howdy disable says only what it has done")
import contextlib  # noqa: E402
import io  # noqa: E402
import runpy  # noqa: E402

disable_config = os.path.join(WORK_DIR, "disable.ini")


def disable(content):
	"""(exit status, the config afterwards) of `howdy disable 1`"""
	with open(disable_config, "w") as handle:
		handle.write(content)
	builtins.howdy_args = types.SimpleNamespace(arguments=["1"])
	originals = os.geteuid, paths_factory.config_file_path
	os.geteuid = lambda: 0
	paths_factory.config_file_path = lambda: disable_config
	try:
		with contextlib.redirect_stdout(io.StringIO()):
			runpy.run_module("howdy.cli.disable")
		code = 0
	except SystemExit as exc:
		code = exc.code
	finally:
		os.geteuid, paths_factory.config_file_path = originals
	with open(disable_config) as handle:
		return code, handle.read()


for name, content, want in (
	(
		"the option is changed",
		"[core]\ndisabled = false\n",
		(0, "[core]\ndisabled = true\n"),
	),
	(
		"written another way: an error",
		"[core]\ndisabled=false\n",
		(1, "[core]\ndisabled=false\n"),
	),
	("no such option: an error", "[core]\n", (1, "[core]\n")),
	(
		"a comment says the same: it stays",
		"[core]\n# disabled = false\ndisabled = false\n",
		(0, "[core]\n# disabled = false\ndisabled = true\n"),
	),
	(
		"only a comment says it that way: an error",
		"[core]\n# disabled = false\ndisabled=false\n",
		(1, "[core]\n# disabled = false\ndisabled=false\n"),
	),
):
	check(name, disable(content), want)

print("--- the howdy program of the Python cli")
# A signal for it has to reach Python: the setup wizard ends `howdy add` with one
with open(os.path.join(SOURCE_DIR, "..", "..", "assets", "bin", "howdy.in")) as handle:
	launcher = handle.read()
stand_in_python = os.path.join(WORK_DIR, "launcher-python")
with open(stand_in_python, "w") as handle:
	handle.write('#!/bin/sh\necho "$$"\n')
os.chmod(stand_in_python, 0o755)
launcher_path = os.path.join(WORK_DIR, "launcher-howdy")
with open(launcher_path, "w") as handle:
	handle.write(launcher.replace("@python_path@", stand_in_python))
os.chmod(launcher_path, 0o755)
with subprocess.Popen([launcher_path], stdout=subprocess.PIPE) as process:
	output = process.stdout.read()
check("Python is the process that was started", int(output), process.pid)

print("--- the camera that is opened when the path does not work")


def opened_cameras(path, node):
	"""What cv2.VideoCapture is asked to open for a path that leads to the node"""
	opened = []

	class Camera:
		def __init__(self, source, *args):
			opened.append(source)

		def isOpened(self):
			return False

		def set(self, *args):
			return True

		def get(self, *args):
			return 0

		def grab(self):
			return True

		def release(self):
			pass

	config = configparser.ConfigParser()
	config.read_dict({"video": {"device_path": path}})
	original = capture_module.cv2.VideoCapture, capture_module.os.path.realpath
	capture_module.cv2.VideoCapture = Camera
	capture_module.os.path.realpath = lambda _: node
	try:
		capture = object.__new__(capture_module.VideoCapture)
		capture.config = config
		capture._create_reader()
	finally:
		capture_module.cv2.VideoCapture, capture_module.os.path.realpath = original
	return opened[1:]


check("a video node: its index", opened_cameras("/dev/video2", "/dev/video2"), [2])
check(
	"a link to a video node: the index of the node",
	opened_cameras("/dev/v4l/by-path/x-video-index0", "/dev/video2"),
	[2],
)
check(
	"another path with a number: no other camera",
	opened_cameras("/tmp/video-index0", "/tmp/video-index0"),
	[],
)

print("--- retention of the snapshots")
from howdy import paths_factory, snapshot  # noqa: E402

DAY = 86400
DIRECTORY = str(paths_factory.snapshots_dir_path())


def make_file(name, age):
	path = os.path.join(DIRECTORY, name)
	with open(path, "w") as handle:
		handle.write("x")
	moment = time.time() - age
	os.utime(path, (moment, moment))


def fill():
	shutil.rmtree(DIRECTORY, ignore_errors=True)
	os.makedirs(DIRECTORY)
	for name, age in (
		("a-10d.jpg", 10 * DAY),
		("b-8d.jpg", 8 * DAY),
		("c-6d.jpg", 6 * DAY),
		("d-3d.jpg", 3 * DAY),
		("e-1d.jpg", 1 * DAY),
		("f-1h.jpg", 3600),
		("notes-20d.txt", 20 * DAY),
	):
		make_file(name, age)
	os.mkdir(os.path.join(DIRECTORY, "dir-20d.jpg"))


OTHERS = {"notes-20d.txt", "dir-20d.jpg"}


def names():
	return set(os.listdir(DIRECTORY))


shutil.rmtree(DIRECTORY, ignore_errors=True)
check("no snapshot directory", snapshot.prune(7 * DAY, 3), 0)

fill()
check("no limit deletes nothing", snapshot.prune(0, 0), 0)
check("no limit keeps all files", len(names()), 8)

check("max age 7d deletes two", snapshot.prune(7 * DAY, 0), 2)
check(
	"max age keeps the younger ones and what is no snapshot",
	names(),
	OTHERS | {"c-6d.jpg", "d-3d.jpg", "e-1d.jpg", "f-1h.jpg"},
)

fill()
check("max 3 snapshots deletes three", snapshot.prune(0, 3), 3)
check("the newest are kept", names(), OTHERS | {"d-3d.jpg", "e-1d.jpg", "f-1h.jpg"})
check("nothing more to delete", snapshot.prune(0, 3), 0)

fill()
check("age and count together", snapshot.prune(2 * DAY, 5), 4)
check("only the newest two are left", names(), OTHERS | {"e-1d.jpg", "f-1h.jpg"})

# A link is not followed and not deleted, its target is not touched
fill()
target = os.path.join(WORK_DIR, "outside.jpg")
with open(target, "w") as handle:
	handle.write("x")
os.utime(target, (time.time() - 30 * DAY,) * 2)
os.symlink(target, os.path.join(DIRECTORY, "link-30d.jpg"))
snapshot.prune(7 * DAY, 0)
check(
	"the link is left alone",
	os.path.islink(os.path.join(DIRECTORY, "link-30d.jpg")),
	True,
)
check("its target is left alone", os.path.exists(target), True)

# The directory itself being a link is refused
shutil.rmtree(DIRECTORY)
elsewhere = os.path.join(WORK_DIR, "elsewhere")
shutil.rmtree(elsewhere, ignore_errors=True)
os.makedirs(elsewhere)
make_file_path = os.path.join(elsewhere, "old.jpg")
with open(make_file_path, "w") as handle:
	handle.write("x")
os.utime(make_file_path, (time.time() - 30 * DAY,) * 2)
os.symlink(elsewhere, DIRECTORY)
try:
	snapshot.prune(7 * DAY, 0)
	refused = False
except OSError:
	refused = True
check("a linked directory is refused", refused, True)
check("and nothing in it is deleted", os.path.exists(make_file_path), True)
os.unlink(DIRECTORY)

print("--- which model files are used")
import stat  # noqa: E402

from howdy.file_util import read_trusted_file  # noqa: E402

ROOT = os.path.join(WORK_DIR, "trusted")
shutil.rmtree(ROOT, ignore_errors=True)
os.makedirs(ROOT)
os.chmod(ROOT, 0o755)


def write(path, mode):
	with open(path, "w") as handle:
		handle.write("content")
	os.chmod(path, mode)


def read(path):
	"""The content, 'missing', or 'refused: ' and the reason"""
	try:
		return read_trusted_file(path)
	except FileNotFoundError:
		return "missing"
	except PermissionError as exc:
		return "refused: " + str(exc).split(": ", 1)[1]


NOT_WRITABLE = "it must belong to root and must not be writable by others"
DIRECTORY_NOT_WRITABLE = (
	"its directory must belong to root and must not be writable by others"
)

check("missing file", read(os.path.join(ROOT, "none.dat")), "missing")
check("missing directory", read(os.path.join(ROOT, "none", "none.dat")), "missing")

write(os.path.join(ROOT, "a.dat"), 0o600)
check("file of the owner", read(os.path.join(ROOT, "a.dat")), "content")
write(os.path.join(ROOT, "b.dat"), 0o644)
check("readable by others is fine", read(os.path.join(ROOT, "b.dat")), "content")
write(os.path.join(ROOT, "c.dat"), 0o660)
check(
	"writable by the group",
	read(os.path.join(ROOT, "c.dat")),
	"refused: " + NOT_WRITABLE,
)
write(os.path.join(ROOT, "d.dat"), 0o602)
check(
	"writable by others", read(os.path.join(ROOT, "d.dat")), "refused: " + NOT_WRITABLE
)

os.symlink(os.path.join(ROOT, "a.dat"), os.path.join(ROOT, "link.dat"))
check(
	"link to a good file", read(os.path.join(ROOT, "link.dat")), "refused: it is a link"
)
os.mkdir(os.path.join(ROOT, "dir.dat"))
check(
	"directory",
	read(os.path.join(ROOT, "dir.dat")),
	"refused: it is not a regular file",
)

os.makedirs(os.path.join(ROOT, "open"))
write(os.path.join(ROOT, "open", "a.dat"), 0o600)
for mode, name, want in (
	(0o777, "directory writable by others", "refused: " + DIRECTORY_NOT_WRITABLE),
	(0o775, "directory writable by the group", "refused: " + DIRECTORY_NOT_WRITABLE),
	(0o711, "directory that can only be searched", "content"),
):
	os.chmod(os.path.join(ROOT, "open"), mode)
	check(name, read(os.path.join(ROOT, "open", "a.dat")), want)

# A directory that is a link is followed, the one it points to is checked
os.symlink(os.path.join(ROOT, "open"), os.path.join(ROOT, "linked"))
check("linked directory", read(os.path.join(ROOT, "linked", "a.dat")), "content")
os.chmod(os.path.join(ROOT, "open"), 0o777)
check(
	"linked directory that is writable by others",
	read(os.path.join(ROOT, "linked", "a.dat")),
	"refused: " + DIRECTORY_NOT_WRITABLE,
)

# Root owns it, the user of the test is not root
check("file of root", not read("/etc/hostname").startswith("refused"), True)
check(
	"the mode of a file is what the test set",
	stat.S_IMODE(os.stat(os.path.join(ROOT, "c.dat")).st_mode),
	0o660,
)

print("--- encryption of the face models")
try:
	import cryptography  # noqa: F401
except ImportError:
	print("SKIP  the Python package 'cryptography' is not installed")
else:
	from howdy.file_util import (
		decrypt_model_file,
		encrypt_all_models,
		encrypt_model_file,
		is_encrypted_model_file,
		read_model_file,
		verify_models_key,
		write_model_file,
	)

	KEY = b"k" * 32
	PLAIN = b'[{"id":0}]'
	# Made by the C++ implementation (model_crypto.cc) for the user alice, so the
	# two implementations keep to the same format
	CPP_FILE = bytes.fromhex(
		"4857444d0125fa9a8ef95061bc5adc8552fab1619d3804bc6140675dbafb19344ebcc151889bc50a6ec591"
	)

	def refused(call):
		try:
			call()
		except (ValueError, RuntimeError) as exc:
			return str(exc)
		return None

	check(
		"a file of the C++ daemon is decrypted",
		decrypt_model_file(KEY, "alice", CPP_FILE),
		PLAIN,
	)
	sealed = encrypt_model_file(KEY, "alice", PLAIN)
	check(
		"a file is recognized",
		(is_encrypted_model_file(sealed), is_encrypted_model_file(PLAIN)),
		(True, False),
	)
	check("no plain text in the file", b"id" in sealed, False)
	check("it is decrypted again", decrypt_model_file(KEY, "alice", sealed), PLAIN)
	check(
		"the nonce is new each time",
		encrypt_model_file(KEY, "alice", PLAIN) != sealed,
		True,
	)
	check(
		"another user is refused",
		refused(lambda: decrypt_model_file(KEY, "bob", sealed)) is not None,
		True,
	)
	check(
		"another key is refused",
		refused(lambda: decrypt_model_file(b"x" * 32, "alice", sealed)) is not None,
		True,
	)
	changed = bytearray(sealed)
	changed[-20] ^= 1
	check(
		"a changed file is refused",
		refused(lambda: decrypt_model_file(KEY, "alice", bytes(changed))) is not None,
		True,
	)
	check(
		"a truncated file is refused",
		refused(lambda: decrypt_model_file(KEY, "alice", sealed[:-1])) is not None,
		True,
	)
	check(
		"an unknown version is refused",
		"version"
		in (
			refused(
				lambda: decrypt_model_file(
					KEY, "alice", sealed[:4] + b"\x02" + sealed[5:]
				)
			)
			or ""
		),
		True,
	)

	# The files and the key credential, as a service gets them
	CRYPTO = os.path.join(WORK_DIR, "crypto")
	shutil.rmtree(CRYPTO, ignore_errors=True)
	for sub in ("", "models", "creds"):
		os.makedirs(os.path.join(CRYPTO, sub), exist_ok=True)
		os.chmod(os.path.join(CRYPTO, sub), 0o700)
	MODELS = os.path.join(CRYPTO, "models")

	def put(path, content):
		with open(path, "wb") as handle:
			handle.write(content)
		os.chmod(path, 0o600)

	def set_key(key):
		path = os.path.join(CRYPTO, "creds", "models-key")
		if os.path.exists(path):
			os.chmod(path, 0o600)
		put(path, key)
		os.environ["CREDENTIALS_DIRECTORY"] = os.path.join(CRYPTO, "creds")

	os.environ.pop("CREDENTIALS_DIRECTORY", None)
	put(os.path.join(MODELS, "carol.dat"), PLAIN)
	put(os.path.join(MODELS, "dave.dat"), b"not json")
	check(
		"without a key a plain file is read",
		read_model_file(os.path.join(MODELS, "carol.dat"), "carol"),
		PLAIN.decode(),
	)
	write_model_file(os.path.join(MODELS, "erin.dat"), "erin", PLAIN.decode())
	with open(os.path.join(MODELS, "erin.dat"), "rb") as handle:
		check("without a key the file stays plain", handle.read(), PLAIN)
	check(
		"without a key nothing is encrypted",
		"credential" in (refused(lambda: encrypt_all_models(MODELS)) or ""),
		True,
	)

	set_key(KEY)
	encrypted, already, failures = encrypt_all_models(MODELS)
	check("the plain files are encrypted", (encrypted, already), (2, 0))
	check(
		"an invalid file is reported", [f.split(":")[0] for f in failures], ["dave.dat"]
	)
	with open(os.path.join(MODELS, "dave.dat"), "rb") as handle:
		check("and it is unchanged", handle.read(), b"not json")
	with open(os.path.join(MODELS, "carol.dat"), "rb") as handle:
		check(
			"the file on the disk is encrypted",
			is_encrypted_model_file(handle.read()),
			True,
		)
	check(
		"it is read as before",
		read_model_file(os.path.join(MODELS, "carol.dat"), "carol"),
		PLAIN.decode(),
	)
	check("a second run changes nothing", encrypt_all_models(MODELS)[:2], (0, 2))
	write_model_file(os.path.join(MODELS, "carol.dat"), "carol", PLAIN.decode())
	with open(os.path.join(MODELS, "carol.dat"), "rb") as handle:
		check(
			"with a key a file is written encrypted",
			is_encrypted_model_file(handle.read()),
			True,
		)
	shutil.copy(os.path.join(MODELS, "carol.dat"), os.path.join(MODELS, "frank.dat"))
	check(
		"the file of another user is refused",
		refused(lambda: read_model_file(os.path.join(MODELS, "frank.dat"), "frank"))
		is not None,
		True,
	)
	# frank.dat is the file of carol, it does not decrypt for another name
	matching, failures = verify_models_key(MODELS, KEY)
	check(
		"the right key decrypts the files",
		(matching, [f.split(":")[0] for f in failures]),
		(2, ["frank.dat"]),
	)
	matching, failures = verify_models_key(MODELS, b"x" * 32)
	check("another key is reported for each file", (matching, len(failures)), (0, 3))
	set_key(b"short")
	check(
		"a key of a wrong size is refused",
		"32 bytes"
		in (
			refused(lambda: read_model_file(os.path.join(MODELS, "carol.dat"), "carol"))
			or ""
		),
		True,
	)
	os.environ.pop("CREDENTIALS_DIRECTORY", None)
	check(
		"without the key an encrypted file is refused",
		"credential"
		in (
			refused(lambda: read_model_file(os.path.join(MODELS, "carol.dat"), "carol"))
			or ""
		),
		True,
	)


sys.exit(1 if failed else 0)

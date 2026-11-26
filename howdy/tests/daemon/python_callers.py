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

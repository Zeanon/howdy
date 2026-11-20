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

sys.exit(1 if failed else 0)

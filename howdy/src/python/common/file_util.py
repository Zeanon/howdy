import os
import json
import errno
import stat

from .caller import is_privileged


def write_file_atomic(path, content, mode=0o600):
	"""Write a file so that a reader sees either the old or the new content, never
	a half written file. The content goes to a temporary file next to the target
	first, which is then renamed over it."""
	temporary = f"{path}.tmp.{os.getpid()}"

	fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, mode)
	try:
		with os.fdopen(fd, "wb" if isinstance(content, bytes) else "w") as file:
			# The mode passed to open() is filtered by the umask
			os.fchmod(file.fileno(), mode)
			file.write(content)
			file.flush()
			os.fsync(file.fileno())

		os.replace(temporary, path)
	except BaseException:
		# Don't leave the temporary file behind
		try:
			os.unlink(temporary)
		except FileNotFoundError:
			pass
		raise


def _is_trustworthy(info):
	"""Belongs to root or the user of the daemon, nobody else can write it"""
	return is_privileged(info.st_uid, True) and not info.st_mode & (
		stat.S_IWGRP | stat.S_IWOTH
	)


def read_trusted_bytes(path):
	"""Read a file that decides who may authenticate, so nobody but root and the
	user of the daemon may be able to change it. The file and its directory must
	belong to one of them and must not be writable by the group or others, and the
	file must be a regular file and not a link. The checks run on the opened file,
	so it can not be swapped in between.

	Raises FileNotFoundError if the file (or its directory) does not exist and
	PermissionError if it is not trusted."""
	path = os.fspath(path)

	# The directory, as it is after resolving links
	directory_fd = os.open(
		os.path.dirname(path) or ".", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC
	)
	try:
		if not _is_trustworthy(os.fstat(directory_fd)):
			raise PermissionError(
				f"Not using {path}: its directory must belong to root and must not be writable by others"
			)

		try:
			fd = os.open(
				os.path.basename(path),
				os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
				dir_fd=directory_fd,
			)
		except OSError as exc:
			if exc.errno == errno.ELOOP:
				raise PermissionError(f"Not using {path}: it is a link") from exc
			raise
	finally:
		os.close(directory_fd)

	try:
		info = os.fstat(fd)
		if not stat.S_ISREG(info.st_mode):
			raise PermissionError(f"Not using {path}: it is not a regular file")
		if not _is_trustworthy(info):
			raise PermissionError(
				f"Not using {path}: it must belong to root and must not be writable by others"
			)
	except BaseException:
		os.close(fd)
		raise

	with os.fdopen(fd, "rb") as file:
		return file.read()


def read_trusted_file(path):
	"""The text of read_trusted_bytes()"""
	return read_trusted_bytes(path).decode()


# <face model encryption>
# Same format as model_crypto.cc: "HWDM", version 1, a 12 byte nonce, the
# ciphertext and the 16 byte tag of AES-256-GCM. The header, the user and the
# file name are authenticated. The key is the systemd credential "models-key",
# which is sealed to the TPM. Without it the files stay plain JSON.
_MAGIC = b"HWDM"
_VERSION = 1
_HEADER = _MAGIC + bytes([_VERSION])
MODELS_KEY_CREDENTIAL = "models-key"


def _aesgcm(key):
	try:
		from cryptography.hazmat.primitives.ciphers.aead import AESGCM
	except ImportError as exc:
		raise RuntimeError(
			"The face model encryption needs the Python package 'cryptography'"
		) from exc
	return AESGCM(key)


def _associated_data(username):
	return _HEADER + username.encode() + b"\0" + username.encode() + b".dat"


def load_models_key():
	"""The key of the credential of this service, None if there is none"""
	directory = os.environ.get("CREDENTIALS_DIRECTORY")
	if not directory:
		return None
	path = os.path.join(directory, MODELS_KEY_CREDENTIAL)
	try:
		key = read_trusted_bytes(path)
	except FileNotFoundError:
		return None
	if len(key) != 32:
		raise ValueError(
			f"The credential {MODELS_KEY_CREDENTIAL} must hold exactly 32 bytes"
		)
	return key


def is_encrypted_model_file(content):
	return content.startswith(_MAGIC)


def encrypt_model_file(key, username, plain):
	nonce = os.urandom(12)
	return (
		_HEADER + nonce + _aesgcm(key).encrypt(nonce, plain, _associated_data(username))
	)


def decrypt_model_file(key, username, content):
	if len(content) < len(_HEADER) + 12 + 16 or not is_encrypted_model_file(content):
		raise ValueError("Face model file is invalid")
	if content[len(_MAGIC)] != _VERSION:
		raise ValueError("Face model file has an unknown version")
	nonce = content[len(_HEADER) : len(_HEADER) + 12]
	try:
		return _aesgcm(key).decrypt(
			nonce, content[len(_HEADER) + 12 :], _associated_data(username)
		)
	except Exception as exc:
		if isinstance(exc, RuntimeError):
			raise
		raise ValueError(
			f"Face model file of {username} was changed or is not for this user"
		) from exc


def read_model_file(path, username):
	"""The models of a user as JSON text. A trusted file, decrypted if it is
	encrypted, a file without the header is the old plain format.

	Raises FileNotFoundError if the user has no file."""
	content = read_trusted_bytes(path)
	if not is_encrypted_model_file(content):
		return content.decode()
	key = load_models_key()
	if key is None:
		raise ValueError(
			f"The face models are encrypted, but the service has no {MODELS_KEY_CREDENTIAL} credential"
			" (see the models_key_file option)"
		)
	return decrypt_model_file(key, username, content).decode()


def write_model_file(path, username, plain):
	"""Atomically write the models of a user, encrypted if there is a key"""
	key = load_models_key()
	if key is None:
		write_file_atomic(path, plain)
	else:
		write_file_atomic(path, encrypt_model_file(key, username, plain.encode()))


# </face model encryption>

import os
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
		with os.fdopen(fd, "w") as file:
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


def read_trusted_file(path):
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

	with os.fdopen(fd, "r") as file:
		return file.read()

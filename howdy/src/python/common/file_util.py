import os


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

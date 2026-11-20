#include "trusted_file.hh"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caller.hh"

namespace howdy {

namespace {

struct Descriptor {
	int fd;
	explicit Descriptor(int fd_) : fd(fd_) {
	}
	~Descriptor() {
		if (fd >= 0) {
			close(fd);
		}
	}
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	Descriptor(Descriptor&&) = delete;
	Descriptor& operator=(Descriptor&&) = delete;
};

// Belongs to root or the user of the daemon, nobody else can write it
bool is_trustworthy(const struct stat& info) {
	return is_privileged(info.st_uid, true) && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

} // namespace

std::optional<std::string> read_trusted_file(const std::filesystem::path& path) {
	// The directory, as it is after resolving links
	const Descriptor directory(open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
	if (directory.fd < 0) {
		if (errno == ENOENT) {
			return std::nullopt;
		}
		throw std::runtime_error("Could not open " + path.parent_path().string() + ": " + std::strerror(errno));
	}
	struct stat directory_info{};
	if (fstat(directory.fd, &directory_info) != 0 || !is_trustworthy(directory_info)) {
		throw std::runtime_error("Not using " + path.string() + ": its directory must belong to root and must not be writable by others");
	}

	const Descriptor file(openat(directory.fd, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	if (file.fd < 0) {
		if (errno == ENOENT) {
			return std::nullopt;
		}
		if (errno == ELOOP) {
			throw std::runtime_error("Not using " + path.string() + ": it is a link");
		}
		throw std::runtime_error("Could not open " + path.string() + ": " + std::strerror(errno));
	}
	struct stat info{};
	if (fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		throw std::runtime_error("Not using " + path.string() + ": it is not a regular file");
	}
	if (!is_trustworthy(info)) {
		throw std::runtime_error("Not using " + path.string() + ": it must belong to root and must not be writable by others");
	}

	std::string content;
	std::array<char, 4096> buffer{};
	ssize_t count = 0;
	while ((count = read(file.fd, buffer.data(), buffer.size())) != 0) {
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error("Could not read " + path.string() + ": " + std::strerror(errno));
		}
		content.append(buffer.data(), static_cast<std::size_t>(count));
	}
	return content;
}

} // namespace howdy

#include "util.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <syslog.h>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace howdy {

long long str_to_seconds(const std::string& value) {
	std::size_t i = (!value.empty() && value[0] == '-') ? 1 : 0;
	while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i])) != 0) {
		++i;
	}

	// std::stoll would silently accept leading whitespace and a "+", reject
	// everything that Python's int() on the same slice would reject as well.
	const std::string number = value.substr(0, i);
	if (number.empty() || number == "-") {
		throw std::invalid_argument("Invalid timeout value: '" + value + "'");
	}
	const long long timeout = std::stoll(number);

	if (i == value.size()) {
		return timeout;
	}

	switch (value[i]) {
		case 'd': return timeout * 86400;
		case 'h': return timeout * 3600;
		case 'm': return timeout * 60;
		default: return timeout;
	}
}

bool str_to_bool(const std::string& value) {
	std::string lower = value;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	static const std::array<const char*, 5> TRUE_VALUES = {"true", "yes", "on", "y", "1"};
	return std::any_of(TRUE_VALUES.begin(), TRUE_VALUES.end(), [&](const char* v) { return lower == v; });
}

double probability_to_certainty(double probability) {
	const double p = std::max(0.0, std::min(100.0, probability));
	if (p <= 50) {
		return 1.0 - (p / 100.0);
	}
	return 0.5 * std::pow((100.0 - p) / 50.0, 0.569323);
}

std::string trim(const std::string& value, const char* chars) {
	const auto begin = value.find_first_not_of(chars);
	if (begin == std::string::npos) {
		return "";
	}
	const auto end = value.find_last_not_of(chars);
	return value.substr(begin, end - begin + 1);
}

std::string to_upper(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return value;
}

void log(int priority, const std::string& message) {
	syslog(priority, "%s", message.c_str());
}

namespace {

// Run a passwd lookup with a buffer that grows until the entry fits
template <typename Lookup, typename Result> std::optional<Result> with_passwd(Lookup lookup, Result (*extract)(const passwd&)) {
	std::vector<char> buffer(1024);
	for (;;) {
		passwd entry{};
		passwd* found = nullptr;
		const int r = lookup(&entry, buffer.data(), buffer.size(), &found);
		if (r == ERANGE && buffer.size() < (1U << 20U)) {
			buffer.resize(buffer.size() * 2);
			continue;
		}
		if (r != 0 || found == nullptr) {
			return std::nullopt;
		}
		return extract(*found);
	}
}

} // namespace

std::optional<uid_t> lookup_uid(const std::string& username) {
	return with_passwd<>(
		[&](passwd* entry, char* buf, std::size_t size, passwd** result) { return getpwnam_r(username.c_str(), entry, buf, size, result); },
		+[](const passwd& entry) { return entry.pw_uid; });
}

std::optional<std::string> lookup_username(uid_t uid) {
	return with_passwd<>(
		[&](passwd* entry, char* buf, std::size_t size, passwd** result) { return getpwuid_r(uid, entry, buf, size, result); },
		+[](const passwd& entry) { return std::string(entry.pw_name); });
}

std::optional<std::string> lookup_home(const std::string& username) {
	return with_passwd<>(
		[&](passwd* entry, char* buf, std::size_t size, passwd** result) { return getpwnam_r(username.c_str(), entry, buf, size, result); },
		+[](const passwd& entry) { return std::string(entry.pw_dir); });
}

std::string read_file(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		throw std::runtime_error("Could not open " + path.string() + ": " + std::strerror(errno));
	}
	std::ostringstream content;
	content << file.rdbuf();
	if (file.bad()) {
		throw std::runtime_error("Could not read " + path.string());
	}
	return content.str();
}

void write_file_atomic(const std::filesystem::path& path, const std::string& content, mode_t mode, std::optional<uid_t> owner, std::optional<gid_t> group) {
	const std::string temporary = path.string() + ".tmp." + std::to_string(getpid());

	const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
	if (fd < 0) {
		throw std::runtime_error("Could not create " + temporary + ": " + std::strerror(errno));
	}

	// However this function is left: the file is closed and the temporary file is
	// removed, unless it replaced the target
	bool closed = false;
	bool replaced = false;
	const ScopeExit cleanup([&] {
		if (!closed) {
			close(fd);
		}
		if (!replaced) {
			unlink(temporary.c_str());
		}
	});

	const auto fail = [&](const std::string& what) { throw std::runtime_error(what + " " + temporary + ": " + std::strerror(errno)); };

	// The mode passed to open() is filtered by the umask
	if (fchmod(fd, mode) != 0) {
		fail("Could not set the permissions of");
	}
	if ((owner.has_value() || group.has_value()) && fchown(fd, owner.value_or(static_cast<uid_t>(-1)), group.value_or(static_cast<gid_t>(-1))) != 0) {
		fail("Could not set the owner of");
	}

	for (std::size_t written = 0; written < content.size();) {
		const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			fail("Could not write");
		}
		written += static_cast<std::size_t>(n);
	}

	if (fsync(fd) != 0) {
		fail("Could not sync");
	}
	close(fd);
	closed = true;

	if (rename(temporary.c_str(), path.c_str()) != 0) {
		throw std::runtime_error("Could not replace " + path.string() + ": " + std::strerror(errno));
	}
	replaced = true;
}

std::string format_double(double value) {
	std::array<char, 64> buffer{};
	const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
	std::string text(buffer.data(), result.ptr);
	if (text.find_first_of(".en") == std::string::npos) {
		text += ".0";
	}
	return text;
}

} // namespace howdy

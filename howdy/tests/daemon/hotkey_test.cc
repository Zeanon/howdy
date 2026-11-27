// Test of the hotkey rubber stamp with keyboards that are stand-ins: libevdev is
// replaced by the functions below, which deliver the key events of a script, and
// opening /dev/input is redirected (linked with --wrap=open). The events of one
// call of the script come between two looks of the stamp, like a key that is
// only tapped.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <libevdev/libevdev.h>

#include "rubberstamps/hotkey.hh"

namespace {

// The key events that are waiting, as (code, value)
std::deque<std::pair<unsigned int, int>> events;
bool failed = false;
int device = 0;

} // namespace

extern "C" {

int __real_open(const char* path, int flags, ...);

int __wrap_open(const char* path, int flags, ...) {
	if (std::strncmp(path, "/dev/input/", 11) == 0) {
		return __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
	}
	va_list arguments;
	va_start(arguments, flags);
	const mode_t mode = (flags & O_CREAT) != 0 ? va_arg(arguments, mode_t) : 0;
	va_end(arguments);
	return __real_open(path, flags, mode);
}

int libevdev_new_from_fd(int /*fd*/, struct libevdev** result) {
	*result = reinterpret_cast<struct libevdev*>(&device);
	return 0;
}

void libevdev_free(struct libevdev* /*dev*/) {
}

int libevdev_has_event_code(const struct libevdev* /*dev*/, unsigned int /*type*/, unsigned int /*code*/) {
	return 1;
}

int libevdev_next_event(struct libevdev* /*dev*/, unsigned int /*flags*/, struct input_event* event) {
	if (events.empty()) {
		return -EAGAIN;
	}
	*event = {};
	event->type = EV_KEY;
	event->code = static_cast<unsigned short>(events.front().first);
	event->value = events.front().second;
	events.pop_front();
	return LIBEVDEV_READ_STATUS_SUCCESS;
}

} // extern "C"

namespace {

using Keys = std::vector<std::pair<unsigned int, int>>;

// Whether the stamp lets the authentication pass when these events are there at its first look
bool passes(const Keys& keys, bool failsafe, const std::string& confirm_key = "enter") {
	events.assign(keys.begin(), keys.end());

	// The stamp uses nothing of the camera and the recognition
	alignas(16) static unsigned char unused[64];
	const std::atomic<bool> cancel{false};
	const howdy::StampContext context{
		*reinterpret_cast<const howdy::Config*>(unused),
		*reinterpret_cast<howdy::VideoCapture*>(unused),
		*reinterpret_cast<howdy::FaceBackend*>(unused),
		{},
		1,
		0,
		cancel,
		[](const std::string&) {},
		[](const std::string&) {},
		{},
		{},
	};
	howdy::HotkeyStamp stamp;
	stamp.options["confirm_key"] = confirm_key;
	const howdy::StampRule rule{"hotkey", 0.4, failsafe, ""};
	return stamp.run(context, rule);
}

void check(const char* name, bool got, bool want) {
	failed = failed || got != want;
	std::printf("%s  %s\n", got == want ? "PASS" : "FAIL", name);
}

} // namespace

int main() {
	std::error_code error;
	bool found = false;
	for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
		found = found || entry.path().filename().string().starts_with("event");
	}
	if (!found) {
		std::puts("SKIP  there is nothing in /dev/input that could stand for a keyboard");
		return 0;
	}

	const Keys enter{{KEY_ENTER, 1}, {KEY_ENTER, 0}};
	const Keys escape{{KEY_ESC, 1}, {KEY_ESC, 0}};
	check("without a key the timeout denies with failsafe", passes({}, true), false);
	check("and passes with faildeadly", passes({}, false), true);
	check("a tapped confirm key passes", passes(enter, true), true);
	check("a tapped abort key denies", passes(escape, false), false);
	Keys both = enter;
	both.insert(both.end(), escape.begin(), escape.end());
	check("abort wins over confirm", passes(both, false), false);
	check("a tapped combination counts", passes({{KEY_LEFTCTRL, 1}, {KEY_Y, 1}, {KEY_Y, 0}, {KEY_LEFTCTRL, 0}}, true, "ctrl+y"), true);
	check("not the keys one after the other", passes({{KEY_Y, 1}, {KEY_Y, 0}, {KEY_LEFTCTRL, 1}, {KEY_LEFTCTRL, 0}}, true, "ctrl+y"), false);
	return failed ? 1 : 0;
}

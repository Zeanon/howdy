#include "rubberstamps/hotkey.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

#include "util.hh"

namespace howdy {

namespace {

using std::chrono::milliseconds;

std::string to_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

// One key of a hotkey, any of the codes counts (ctrl is the left or the right key)
using KeyGroup = std::vector<int>;
// All groups have to be pressed together, like ctrl+shift+a
using Hotkey = std::vector<KeyGroup>;

// Parse a hotkey like "esc", "enter" or "ctrl+alt+x", std::nullopt for unknown keys
std::optional<Hotkey> parse_hotkey(const std::string& text) {
	static const std::map<std::string, KeyGroup> ALIASES = {
		{"ctrl", {KEY_LEFTCTRL, KEY_RIGHTCTRL}},
		{"control", {KEY_LEFTCTRL, KEY_RIGHTCTRL}},
		{"shift", {KEY_LEFTSHIFT, KEY_RIGHTSHIFT}},
		{"alt", {KEY_LEFTALT, KEY_RIGHTALT}},
		{"altgr", {KEY_RIGHTALT}},
		{"windows", {KEY_LEFTMETA, KEY_RIGHTMETA}},
		{"super", {KEY_LEFTMETA, KEY_RIGHTMETA}},
		{"meta", {KEY_LEFTMETA, KEY_RIGHTMETA}},
		{"esc", {KEY_ESC}},
		{"escape", {KEY_ESC}},
		{"enter", {KEY_ENTER, KEY_KPENTER}},
		{"return", {KEY_ENTER, KEY_KPENTER}},
		{"del", {KEY_DELETE}},
		{"space", {KEY_SPACE}},
	};

	Hotkey hotkey;
	std::stringstream stream(text);
	std::string token;
	while (std::getline(stream, token, '+')) {
		token = to_lower(trim(token));
		if (token.empty()) {
			return std::nullopt;
		}

		if (const auto alias = ALIASES.find(token); alias != ALIASES.end()) {
			hotkey.push_back(alias->second);
			continue;
		}

		// Everything else by the name of the Linux key code: a, f1, up, backspace, ...
		std::string name = "KEY_";
		for (const char c : token) {
			name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		const int code = libevdev_event_code_from_name(EV_KEY, name.c_str());
		if (code < 0) {
			return std::nullopt;
		}
		hotkey.push_back({code});
	}
	if (hotkey.empty()) {
		return std::nullopt;
	}
	return hotkey;
}

// Reads the key presses of all keyboards through evdev
class KeyboardReader {
  public:
	KeyboardReader() {
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
			if (!entry.path().filename().string().starts_with("event")) {
				continue;
			}

			const int fd = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) {
				continue;
			}

			libevdev* device = nullptr;
			if (libevdev_new_from_fd(fd, &device) < 0) {
				close(fd);
				continue;
			}

			// Only keyboards: anything that has letter keys
			if (libevdev_has_event_code(device, EV_KEY, KEY_A) == 0 || libevdev_has_event_code(device, EV_KEY, KEY_ENTER) == 0) {
				libevdev_free(device);
				close(fd);
				continue;
			}
			devices_.emplace_back(fd, device);
		}
	}

	~KeyboardReader() {
		for (const auto& [fd, device] : devices_) {
			libevdev_free(device);
			close(fd);
		}
	}

	KeyboardReader(const KeyboardReader&) = delete;
	KeyboardReader& operator=(const KeyboardReader&) = delete;
	KeyboardReader(KeyboardReader&&) = delete;
	KeyboardReader& operator=(KeyboardReader&&) = delete;

	[[nodiscard]] bool empty() const {
		return devices_.empty();
	}

	// Handle the events since the last call. `on_press` is called for every key that was pressed, at
	// that moment: a key that is let go again before the next call is pressed during the call all the same.
	void poll(const std::function<void(int)>& on_press) {
		for (const auto& [fd, device] : devices_) {
			unsigned int flags = LIBEVDEV_READ_FLAG_NORMAL;
			input_event event{};
			while (true) {
				const int status = libevdev_next_event(device, flags, &event);
				if (status < 0) {
					break;
				}
				// After the buffer overflowed the missed events have to be read in sync mode
				flags = status == LIBEVDEV_READ_STATUS_SYNC ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;

				if (event.type != EV_KEY) {
					continue;
				}
				if (event.value == 1) {
					pressed_.insert(static_cast<int>(event.code));
					on_press(static_cast<int>(event.code));
				} else if (event.value == 0) {
					pressed_.erase(static_cast<int>(event.code));
				}
			}
		}
	}

	[[nodiscard]] bool is_pressed(int code) const {
		return pressed_.contains(code);
	}

  private:
	std::vector<std::pair<int, libevdev*>> devices_;
	std::set<int> pressed_;
};

// Whether the press of this key completed the hotkey
bool hotkey_triggered(const Hotkey& hotkey, const KeyboardReader& reader, int press) {
	const bool in_hotkey =
		std::any_of(hotkey.begin(), hotkey.end(), [&](const KeyGroup& group) { return std::find(group.begin(), group.end(), press) != group.end(); });
	if (!in_hotkey) {
		return false;
	}
	return std::all_of(hotkey.begin(), hotkey.end(), [&](const KeyGroup& group) {
		return std::any_of(group.begin(), group.end(), [&](int code) { return reader.is_pressed(code); });
	});
}

} // namespace

HotkeyStamp::HotkeyStamp() {
	options["abort_key"] = std::string("esc");
	options["confirm_key"] = std::string("enter");
}

bool HotkeyStamp::run(const StampContext& context, const StampRule& rule) {
	const auto abort_key = parse_hotkey(get_string("abort_key"));
	const auto confirm_key = parse_hotkey(get_string("confirm_key"));
	if (!abort_key.has_value() || !confirm_key.has_value()) {
		log(LOG_ERR, "Unknown key in the hotkey rubber stamp: abort_key = " + get_string("abort_key") + ", confirm_key = " + get_string("confirm_key"));
		return false;
	}

	double time_left = rule.timeout;
	const std::string time_text = rule.failsafe ? S("Aborting authorisation in ") : S("Authorising in ");

	// Set the ui to the default strings
	context.set_message(time_text + std::to_string(static_cast<int>(time_left)));
	context.set_subtext("Press " + get_string("abort_key") + " to abort, " + get_string("confirm_key") + " to authorise");

	KeyboardReader reader;
	if (reader.empty()) {
		log(LOG_ERR, "The hotkey rubber stamp found no keyboard it can read in /dev/input");
		return false;
	}

	while (time_left > 0) {
		// Remove 0.1 seconds from the timer, as that is how long we sleep
		time_left -= 0.1;
		context.set_message(time_text + std::to_string(static_cast<int>(time_left) + 1));

		bool aborted = false;
		bool confirmed = false;
		reader.poll([&](int press) {
			aborted = aborted || hotkey_triggered(*abort_key, reader, press);
			confirmed = confirmed || hotkey_triggered(*confirm_key, reader, press);
		});

		// If the abort key was pressed while the loop was sleeping
		if (aborted) {
			context.set_message(S("Authentication aborted"));
			context.set_subtext("");
			cancelable_sleep(context, milliseconds(1000));
			return false;
		}

		// The authentication can continue if confirm was pressed
		if (confirmed) {
			return true;
		}

		// Wait a bit and check again
		cancelable_sleep(context, milliseconds(100));
	}

	// When the timeout hits, either abort or continue based on failsafe or faildeadly
	return !rule.failsafe;
}

} // namespace howdy

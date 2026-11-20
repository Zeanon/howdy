// Test of the retention of the snapshots: prune_snapshots() in the snapshot
// directory of the paths of the tests.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "paths.hh"
#include "snapshot.hh"

namespace fs = std::filesystem;

namespace {

bool failed = false;

std::string show(bool value) {
	return value ? "true" : "false";
}

std::string show(std::size_t value) {
	return std::to_string(value);
}

std::string show(const std::set<std::string>& values) {
	std::string result = "{";
	for (const auto& value : values) {
		result += (result.size() > 1 ? ", " : "") + value;
	}
	return result + "}";
}

template <typename T> void check(const std::string& name, const T& got, const T& want) {
	const bool ok = got == want;
	failed = failed || !ok;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
	if (!ok) {
		std::printf("      got %s, expected %s\n", show(got).c_str(), show(want).c_str());
	}
}

const fs::path DIRECTORY = fs::path(howdy::paths::LOG_PATH) / "snapshots";
constexpr long HOUR = 3600;
constexpr long DAY = 24 * HOUR;

void make_file(const std::string& name, long age_seconds) {
	const fs::path path = DIRECTORY / name;
	std::ofstream(path) << "x";
	const time_t when = time(nullptr) - age_seconds;
	const utimbuf times{when, when};
	utime(path.c_str(), &times);
}

std::set<std::string> names() {
	std::set<std::string> result;
	for (const auto& entry : fs::directory_iterator(DIRECTORY)) {
		result.insert(entry.path().filename().string());
	}
	return result;
}

// Six snapshots of different age and things that are no snapshots
void fill() {
	fs::remove_all(DIRECTORY);
	fs::create_directories(DIRECTORY);
	make_file("a-10d.jpg", 10 * DAY);
	make_file("b-8d.jpg", 8 * DAY);
	make_file("c-6d.jpg", 6 * DAY);
	make_file("d-3d.jpg", 3 * DAY);
	make_file("e-1d.jpg", 1 * DAY);
	make_file("f-1h.jpg", HOUR);
	make_file("notes-20d.txt", 20 * DAY);
	fs::create_directory(DIRECTORY / "dir-20d.jpg");
}

using Names = std::set<std::string>;
const Names OTHERS{"notes-20d.txt", "dir-20d.jpg"};

Names with_others(Names snapshots) {
	snapshots.insert(OTHERS.begin(), OTHERS.end());
	return snapshots;
}

} // namespace

int main() {
	using howdy::SnapshotRetention;
	using std::chrono::seconds;

	fs::remove_all(DIRECTORY);
	check<std::size_t>("no snapshot directory", howdy::prune_snapshots({seconds(7 * DAY), 3}), 0);

	fill();
	check<std::size_t>("no limit deletes nothing", howdy::prune_snapshots({seconds(0), 0}), 0);
	check("no limit keeps all files", names().size(), static_cast<std::size_t>(8));

	check<std::size_t>("max age 7d deletes two", howdy::prune_snapshots({seconds(7 * DAY), 0}), 2);
	check("max age keeps the younger ones and what is no snapshot", names(), with_others({"c-6d.jpg", "d-3d.jpg", "e-1d.jpg", "f-1h.jpg"}));

	fill();
	check<std::size_t>("max 3 snapshots deletes three", howdy::prune_snapshots({seconds(0), 3}), 3);
	check("the newest are kept", names(), with_others({"d-3d.jpg", "e-1d.jpg", "f-1h.jpg"}));
	check<std::size_t>("nothing more to delete", howdy::prune_snapshots({seconds(0), 3}), 0);

	fill();
	check<std::size_t>("age and count together", howdy::prune_snapshots({seconds(2 * DAY), 5}), 4);
	check("only the newest two are left", names(), with_others({"e-1d.jpg", "f-1h.jpg"}));

	// A link is not followed and not deleted, its target is not touched
	fill();
	const fs::path target = fs::path(howdy::paths::LOG_PATH) / "outside.jpg";
	std::ofstream(target) << "x";
	const time_t old = time(nullptr) - 30 * DAY;
	const utimbuf times{old, old};
	utime(target.c_str(), &times);
	fs::create_symlink(target, DIRECTORY / "link-30d.jpg");
	howdy::prune_snapshots({seconds(7 * DAY), 0});
	check("the link is left alone", fs::is_symlink(DIRECTORY / "link-30d.jpg"), true);
	check("its target is left alone", fs::exists(target), true);

	// The directory itself being a link is refused
	fs::remove_all(DIRECTORY);
	const fs::path elsewhere = fs::path(howdy::paths::LOG_PATH) / "elsewhere";
	fs::create_directories(elsewhere);
	std::ofstream(elsewhere / "old.jpg") << "x";
	utime((elsewhere / "old.jpg").c_str(), &times);
	fs::create_directory_symlink(elsewhere, DIRECTORY);
	bool refused = false;
	try {
		howdy::prune_snapshots({seconds(7 * DAY), 0});
	} catch (const std::exception&) {
		refused = true;
	}
	check("a linked directory is refused", refused, true);
	check("and nothing in it is deleted", fs::exists(elsewhere / "old.jpg"), true);

	return failed ? 1 : 0;
}

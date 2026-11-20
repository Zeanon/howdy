// Test of read_trusted_file(): which files are used for the models.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>

#include "paths.hh"
#include "trusted_file.hh"

namespace fs = std::filesystem;

namespace {

bool failed = false;

void check(const std::string& name, const std::string& got, const std::string& want) {
	const bool ok = got == want;
	failed = failed || !ok;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
	if (!ok) {
		std::printf("      got '%s', expected '%s'\n", got.c_str(), want.c_str());
	}
}

// The content, "missing", or "refused: " and the reason
std::string outcome(const fs::path& path) {
	try {
		const auto content = howdy::read_trusted_file(path);
		return content.has_value() ? *content : "missing";
	} catch (const std::exception& exc) {
		const std::string text = exc.what();
		return "refused: " + text.substr(text.find(": ") + 2);
	}
}

const fs::path ROOT = fs::path(howdy::paths::LOG_PATH) / "trusted";

void write(const fs::path& path, mode_t mode) {
	std::ofstream(path) << "content";
	chmod(path.c_str(), mode);
}

} // namespace

int main() {
	fs::remove_all(ROOT);
	fs::create_directories(ROOT);
	chmod(ROOT.c_str(), 0755);

	check("missing file", outcome(ROOT / "none.dat"), "missing");
	check("missing directory", outcome(ROOT / "none" / "none.dat"), "missing");

	write(ROOT / "a.dat", 0600);
	check("file of the owner", outcome(ROOT / "a.dat"), "content");
	write(ROOT / "b.dat", 0644);
	check("readable by others is fine", outcome(ROOT / "b.dat"), "content");

	write(ROOT / "c.dat", 0660);
	check("writable by the group", outcome(ROOT / "c.dat"), "refused: it must belong to root and must not be writable by others");
	write(ROOT / "d.dat", 0602);
	check("writable by others", outcome(ROOT / "d.dat"), "refused: it must belong to root and must not be writable by others");

	fs::create_symlink(ROOT / "a.dat", ROOT / "link.dat");
	check("link to a good file", outcome(ROOT / "link.dat"), "refused: it is a link");
	fs::create_directory(ROOT / "dir.dat");
	check("directory", outcome(ROOT / "dir.dat"), "refused: it is not a regular file");

	fs::create_directories(ROOT / "open");
	write(ROOT / "open" / "a.dat", 0600);
	chmod((ROOT / "open").c_str(), 0777);
	check("directory writable by others", outcome(ROOT / "open" / "a.dat"), "refused: its directory must belong to root and must not be writable by others");
	chmod((ROOT / "open").c_str(), 0775);
	check("directory writable by the group", outcome(ROOT / "open" / "a.dat"), "refused: its directory must belong to root and must not be writable by others");
	chmod((ROOT / "open").c_str(), 0711);
	check("directory that can only be searched", outcome(ROOT / "open" / "a.dat"), "content");

	// A directory that is a link is followed, the one it points to is checked
	fs::create_directory_symlink(ROOT / "open", ROOT / "linked");
	check("linked directory", outcome(ROOT / "linked" / "a.dat"), "content");
	chmod((ROOT / "open").c_str(), 0777);
	check(
		"linked directory that is writable by others", outcome(ROOT / "linked" / "a.dat"),
		"refused: its directory must belong to root and must not be writable by others");

	// Root owns it, the user of the test is not root
	check("file of root", outcome("/etc/hostname").starts_with("refused") ? "refused" : "used", "used");

	return failed ? 1 : 0;
}

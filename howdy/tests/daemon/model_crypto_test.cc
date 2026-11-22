// Test of the encryption of the face model files: the format, what is refused,
// and the ModelStore with and without the key credential.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>

#include "management/model_store.hh"
#include "model_crypto.hh"

namespace fs = std::filesystem;

namespace {

bool failed = false;

void check(const std::string& name, bool ok) {
	failed = failed || !ok;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
}

template <typename F> std::string error_of(F&& call) {
	try {
		call();
	} catch (const std::exception& exc) {
		return exc.what();
	}
	return "";
}

std::string slurp(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

void spit(const fs::path& path, const std::string& content) {
	std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		return 2;
	}
	const fs::path root = argv[1];
	fs::remove_all(root);
	fs::create_directories(root / "models");
	fs::create_directories(root / "creds");
	chmod(root.c_str(), 0700);
	chmod((root / "models").c_str(), 0700);
	chmod((root / "creds").c_str(), 0700);

	const std::string key(32, 'k');
	const std::string plain = R"([{"id":0,"label":"a","time":1,"data":[[1.5,2.5]]}])";

	// The format
	const auto sealed = howdy::encrypt_model_file(key, "alice", plain);
	check("the file is recognized", howdy::is_encrypted_model_file(sealed) && !howdy::is_encrypted_model_file(plain));
	check("no plain text in the file", sealed.find("label") == std::string::npos);
	check("it is decrypted again", howdy::decrypt_model_file(key, "alice", sealed) == plain);
	check("the nonce is new each time", howdy::encrypt_model_file(key, "alice", plain) != sealed);
	check("another user is refused", !error_of([&] { howdy::decrypt_model_file(key, "bob", sealed); }).empty());
	check("another key is refused", !error_of([&] { howdy::decrypt_model_file(std::string(32, 'x'), "alice", sealed); }).empty());
	auto changed = sealed;
	changed[changed.size() - 20] ^= 1;
	check("a changed file is refused", !error_of([&] { howdy::decrypt_model_file(key, "alice", changed); }).empty());
	auto truncated = sealed.substr(0, sealed.size() - 1);
	check("a truncated file is refused", !error_of([&] { howdy::decrypt_model_file(key, "alice", truncated); }).empty());
	auto other_version = sealed;
	other_version[4] = 2;
	check("an unknown version is refused", error_of([&] { howdy::decrypt_model_file(key, "alice", other_version); }).find("version") != std::string::npos);
	check("a short key is refused", !error_of([&] { howdy::encrypt_model_file("short", "alice", plain); }).empty());

	// The store, without a key: the old plain files
	unsetenv("CREDENTIALS_DIRECTORY");
	const howdy::ModelStore store(root / "models");
	const nlohmann::json model = {{"time", 1}, {"label", "a"}, {"data", nlohmann::json::array({nlohmann::json::array({1.5, 2.5})})}};
	store.add("alice", model);
	check("without a key the file is plain", slurp(root / "models/alice.dat").find("\"label\"") != std::string::npos);

	// With the key: the old file is read and written encrypted from now on
	spit(root / "creds/models-key", key);
	chmod((root / "creds/models-key").c_str(), 0400);
	setenv("CREDENTIALS_DIRECTORY", (root / "creds").c_str(), 1);
	check("a plain file is still read", store.list("alice").find("\"label\":\"a\"") != std::string::npos);
	store.add("alice", model);
	const auto stored = slurp(root / "models/alice.dat");
	check("with a key the file is encrypted", howdy::is_encrypted_model_file(stored));
	check("both models are in it", store.list("alice").find("\"id\":1") != std::string::npos);
	store.add("bob", model);
	check("a file of another user is refused", !error_of([&] {
													fs::copy_file(root / "models/bob.dat", root / "models/alice.dat", fs::copy_options::overwrite_existing);
													(void)store.list("alice");
												}).empty());
	spit(root / "models/alice.dat", stored);
	chmod((root / "models/alice.dat").c_str(), 0600);
	store.remove("alice", "0");
	check(
		"remove keeps it encrypted",
		howdy::is_encrypted_model_file(slurp(root / "models/alice.dat")) && store.list("alice").find("\"id\":0") == std::string::npos);

	// The helper command: plain files are converted, the rest is left alone
	spit(root / "models/carol.dat", plain);
	chmod((root / "models/carol.dat").c_str(), 0600);
	spit(root / "models/dave.dat", "not json");
	chmod((root / "models/dave.dat").c_str(), 0600);
	const auto report = store.encrypt_all();
	check("a plain file is encrypted", report.encrypted == 1 && howdy::is_encrypted_model_file(slurp(root / "models/carol.dat")));
	check("an encrypted file is counted", report.already_encrypted == 2);
	check("an invalid file is reported and unchanged", report.failures.size() == 1 && slurp(root / "models/dave.dat") == "not json");
	check("the encrypted file is read as before", store.list("carol").find("\"label\":\"a\"") != std::string::npos);
	check("a second run changes nothing", store.encrypt_all().encrypted == 0);
	fs::remove(root / "models/dave.dat");

	// The check of a recovery key
	const auto right = store.verify_key(key);
	check("the right key decrypts the files", right.matching == 3 && right.failures.empty());
	const auto wrong = store.verify_key(std::string(32, 'x'));
	check("another key is reported for each file", wrong.matching == 0 && wrong.failures.size() == 3);

	// The key is gone (another TPM): refused, and never read as plain
	unsetenv("CREDENTIALS_DIRECTORY");
	check("without the key an encrypted file is refused", error_of([&] { (void)store.list("alice"); }).find("credential") != std::string::npos);
	setenv("CREDENTIALS_DIRECTORY", (root / "creds").c_str(), 1);
	fs::remove(root / "creds/models-key");
	spit(root / "creds/models-key", "short");
	chmod((root / "creds/models-key").c_str(), 0400);
	check("a key of a wrong size is refused", error_of([&] { (void)store.list("alice"); }).find("32 bytes") != std::string::npos);

	unsetenv("CREDENTIALS_DIRECTORY");
	check("without a key nothing is encrypted", error_of([&] { (void)store.encrypt_all(); }).find("credential") != std::string::npos);

	fs::remove_all(root);
	return failed ? 1 : 0;
}

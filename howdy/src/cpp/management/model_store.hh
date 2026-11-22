#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace howdy {

// The face model file of a user: a JSON list of models, each one holding
// "id", "time", "label" and "data", the list of stored encodings. All
// methods throw std::runtime_error with a message fit for the user.
// What encrypt_all() did
struct EncryptionReport {
	std::size_t encrypted = 0;
	std::size_t already_encrypted = 0;
	// One message per file that could not be converted, those files are unchanged
	std::vector<std::string> failures;
};

class ModelStore {
  public:
	explicit ModelStore(std::filesystem::path directory);

	// Store a new model of the user and give it the next free id
	void add(const std::string& username, nlohmann::json model) const;

	// JSON list with the "time", "label" and "id" of every model
	[[nodiscard]] std::string list(const std::string& username) const;

	void remove(const std::string& username, const std::string& model_id) const;

	// Delete all models of the user
	void clear(const std::string& username) const;

	// Encrypt the model files that are still plain JSON. Needs the key credential,
	// throws std::runtime_error without it. Each new file is read back before it
	// replaces the old one, an encrypted file is only checked to be readable.
	[[nodiscard]] EncryptionReport encrypt_all() const;

  private:
	[[nodiscard]] std::filesystem::path path_for(const std::string& username) const;

	std::filesystem::path directory_;
};

} // namespace howdy

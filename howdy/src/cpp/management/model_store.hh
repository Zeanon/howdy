#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "face_match.hh"

namespace howdy {

// How the face is compared with the stored encodings: dlib encodings by their
// euclidean distance (the smaller the better), OpenVINO encodings by the dot
// product (the larger the better)
enum class Metric { Euclidean, Dot };

// What encrypt_all() did
struct EncryptionReport {
	std::size_t encrypted = 0;
	std::size_t already_encrypted = 0;
	// One message per file that could not be converted, those files are unchanged
	std::vector<std::string> failures;
};

// What verify_key() found
struct KeyCheck {
	// Encrypted files that the key decrypts
	std::size_t matching = 0;
	// One message per encrypted file that it does not decrypt
	std::vector<std::string> failures;
};

// The face model file of a user: a JSON list of models, each one holding
// "id", "time", "label" and "data", the list of stored encodings. All
// methods throw std::runtime_error with a message fit for the user.
class ModelStore {
  public:
	explicit ModelStore(std::filesystem::path directory);

	// Store a new model of the user and give it the next free id
	void add(const std::string& username, nlohmann::json model) const;

	// JSON list with the "time", "label" and "id" of every model
	[[nodiscard]] std::string list(const std::string& username) const;

	// The model is only removed if it has the label and time the caller saw: the ids move up when a
	// model is removed, a caller with an old list would remove another one. The time is the number
	// of seconds as text, like the id.
	void remove(const std::string& username, const std::string& model_id, const std::string& expected_label, const std::string& expected_time) const;

	// Give the model another label. The id is compared as text, like in remove().
	void rename(
		const std::string& username, const std::string& model_id, const std::string& expected_label, const std::string& expected_time,
		const std::string& label) const;

	// Delete all models of the user
	void clear(const std::string& username) const;

	// Encrypt the model files that are still plain JSON. Needs the key credential,
	// throws std::runtime_error without it. Each new file is read back before it
	// replaces the old one, an encrypted file is only checked to be readable.
	[[nodiscard]] EncryptionReport encrypt_all() const;

	// Compare a face with the encodings of the user that have its size. std::nullopt if the
	// user has none. The encodings are not given out, only the best value and the label.
	[[nodiscard]] std::optional<FaceMatch> compare(const std::string& username, Metric metric, const std::vector<double>& face) const;

	// Whether a key (32 bytes) decrypts the encrypted model files, used to check a recovery key
	// before it replaces the sealed one. Plain files are not looked at.
	[[nodiscard]] KeyCheck verify_key(const std::string& key) const;

  private:
	[[nodiscard]] std::filesystem::path path_for(const std::string& username) const;

	std::filesystem::path directory_;
};

} // namespace howdy

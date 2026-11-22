#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace howdy {

// Encryption of the face model files. The key is a systemd credential
// (LoadCredentialEncrypted=), which is sealed to the TPM, so it is never on the
// disk in the clear. Without the credential the files stay plain JSON.
//
// File format: "HWDM", version 1, a random 12 byte nonce, the ciphertext and the
// 16 byte tag of AES-256-GCM. The header, the user name and the file name are
// authenticated, so a file can not be passed off as the file of another user.

// Name of the credential that holds the 32 byte key
inline constexpr const char* MODELS_KEY_CREDENTIAL = "models-key";

// The key of the credential of this service, std::nullopt if there is none.
// Throws std::runtime_error if the credential exists but is not usable.
std::optional<std::string> load_models_key();

// Whether the content of a file is an encrypted model file
bool is_encrypted_model_file(const std::string& content);

std::string encrypt_model_file(const std::string& key, const std::string& username, const std::string& plain);

// Throws std::runtime_error if the content was changed or is not for the user
std::string decrypt_model_file(const std::string& key, const std::string& username, const std::string& content);

// The models of a user as JSON text, std::nullopt if the user has no file. Reads a
// trusted file and decrypts it, a file without the header is the old plain format.
// Throws if the file is encrypted but there is no key.
std::optional<std::string> read_model_file(const std::filesystem::path& path, const std::string& username);

// Atomically write the models of a user, encrypted if there is a key
void write_model_file(const std::filesystem::path& path, const std::string& username, const std::string& plain);

} // namespace howdy

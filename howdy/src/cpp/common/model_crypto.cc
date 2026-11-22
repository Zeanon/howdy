#include "model_crypto.hh"

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "trusted_file.hh"
#include "util.hh"

namespace howdy {

namespace {

constexpr std::string_view MAGIC = "HWDM";
constexpr unsigned char VERSION = 1;
constexpr std::size_t HEADER_SIZE = 5;
constexpr std::size_t KEY_SIZE = 32;
constexpr std::size_t NONCE_SIZE = 12;
constexpr std::size_t TAG_SIZE = 16;
constexpr mode_t MODEL_FILE_MODE = 0600;

struct CipherContextDeleter {
	void operator()(EVP_CIPHER_CTX* context) const {
		EVP_CIPHER_CTX_free(context);
	}
};
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;

std::string header() {
	return std::string(MAGIC) + static_cast<char>(VERSION);
}

// The header, the user and the file name are authenticated
std::string associated_data(const std::string& username) {
	return header() + username + '\0' + username + ".dat";
}

const unsigned char* bytes(const std::string& text) {
	return reinterpret_cast<const unsigned char*>(text.data());
}

} // namespace

std::optional<std::string> load_models_key() {
	const char* directory = std::getenv("CREDENTIALS_DIRECTORY");
	if (directory == nullptr || *directory == '\0') {
		return std::nullopt;
	}
	const auto path = std::filesystem::path(directory) / MODELS_KEY_CREDENTIAL;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		return std::nullopt;
	}
	// systemd hands the credentials out in a directory only root can read
	const auto key = read_trusted_file(path);
	if (!key.has_value() || key->size() != KEY_SIZE) {
		throw std::runtime_error("The credential " + std::string(MODELS_KEY_CREDENTIAL) + " must hold exactly 32 bytes");
	}
	return key;
}

bool is_encrypted_model_file(const std::string& content) {
	return content.size() >= HEADER_SIZE && content.starts_with(MAGIC);
}

std::string encrypt_model_file(const std::string& key, const std::string& username, const std::string& plain) {
	if (key.size() != KEY_SIZE) {
		throw std::runtime_error("Invalid key for the face models");
	}
	std::array<unsigned char, NONCE_SIZE> nonce{};
	if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
		throw std::runtime_error("Could not create a nonce for the face models");
	}

	const CipherContext context(EVP_CIPHER_CTX_new());
	if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, bytes(key), nonce.data()) != 1) {
		throw std::runtime_error("Could not set up the encryption of the face models");
	}

	const std::string aad = associated_data(username);
	int length = 0;
	if (EVP_EncryptUpdate(context.get(), nullptr, &length, bytes(aad), static_cast<int>(aad.size())) != 1) {
		throw std::runtime_error("Could not encrypt the face models");
	}

	std::string cipher(plain.size(), '\0');
	int cipher_length = 0;
	if (!plain.empty() &&
		EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(cipher.data()), &cipher_length, bytes(plain), static_cast<int>(plain.size())) != 1) {
		throw std::runtime_error("Could not encrypt the face models");
	}
	int final_length = 0;
	std::array<unsigned char, TAG_SIZE> tag{};
	if (EVP_EncryptFinal_ex(context.get(), nullptr, &final_length) != 1 ||
		EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
		throw std::runtime_error("Could not encrypt the face models");
	}

	std::string result = header();
	result.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
	result.append(cipher, 0, static_cast<std::size_t>(cipher_length));
	result.append(reinterpret_cast<const char*>(tag.data()), tag.size());
	return result;
}

std::string decrypt_model_file(const std::string& key, const std::string& username, const std::string& content) {
	if (key.size() != KEY_SIZE) {
		throw std::runtime_error("Invalid key for the face models");
	}
	if (content.size() < HEADER_SIZE + NONCE_SIZE + TAG_SIZE || !is_encrypted_model_file(content)) {
		throw std::runtime_error("Face model file is invalid");
	}
	if (static_cast<unsigned char>(content[MAGIC.size()]) != VERSION) {
		throw std::runtime_error("Face model file has an unknown version");
	}

	const std::size_t cipher_size = content.size() - HEADER_SIZE - NONCE_SIZE - TAG_SIZE;
	const std::string nonce = content.substr(HEADER_SIZE, NONCE_SIZE);
	const std::string cipher = content.substr(HEADER_SIZE + NONCE_SIZE, cipher_size);
	std::string tag = content.substr(HEADER_SIZE + NONCE_SIZE + cipher_size);

	const CipherContext context(EVP_CIPHER_CTX_new());
	if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, bytes(key), bytes(nonce)) != 1) {
		throw std::runtime_error("Could not set up the decryption of the face models");
	}

	const std::string aad = associated_data(username);
	int length = 0;
	if (EVP_DecryptUpdate(context.get(), nullptr, &length, bytes(aad), static_cast<int>(aad.size())) != 1) {
		throw std::runtime_error("Could not decrypt the face models");
	}
	std::string plain(cipher_size, '\0');
	int plain_length = 0;
	if (cipher_size != 0 &&
		EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plain.data()), &plain_length, bytes(cipher), static_cast<int>(cipher_size)) != 1) {
		throw std::runtime_error("Could not decrypt the face models");
	}
	int final_length = 0;
	if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) != 1 ||
		EVP_DecryptFinal_ex(context.get(), nullptr, &final_length) != 1) {
		throw std::runtime_error("Face model file of " + username + " was changed or is not for this user");
	}
	plain.resize(static_cast<std::size_t>(plain_length));
	return plain;
}

std::optional<std::string> read_model_file(const std::filesystem::path& path, const std::string& username) {
	auto content = read_trusted_file(path);
	if (!content.has_value() || !is_encrypted_model_file(*content)) {
		// Missing, or a file of the time before the encryption
		return content;
	}
	const auto key = load_models_key();
	if (!key.has_value()) {
		throw std::runtime_error(
			"The face models are encrypted, but the service has no " + std::string(MODELS_KEY_CREDENTIAL) +
			" credential (see the models_key_credential option)");
	}
	return decrypt_model_file(*key, username, *content);
}

void write_model_file(const std::filesystem::path& path, const std::string& username, const std::string& plain) {
	const auto key = load_models_key();
	write_file_atomic(path, key.has_value() ? encrypt_model_file(*key, username, plain) : plain, MODEL_FILE_MODE);
}

} // namespace howdy

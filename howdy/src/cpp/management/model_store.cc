#include "model_store.hh"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <sys/stat.h>

#include "model_crypto.hh"
#include "trusted_file.hh"
#include "util.hh"

namespace howdy {

namespace {

// Only root can look into the directory, so nobody else can even tell who has a
// face model. The PAM module does not look for the model itself, it asks the
// daemon, which runs as root.
constexpr mode_t MODELS_DIR_MODE = 0700;

// A user may only fill so much of the disk of root. The limits are far above
// what howdy needs: a model has up to 13 encodings with 128 (dlib) or 256
// (OpenVINO) values, and the label is limited to 24 characters by `howdy add`.
constexpr std::size_t MAX_LABEL_BYTES = 256;
constexpr std::size_t MAX_ENCODINGS_PER_MODEL = 64;
constexpr std::size_t MAX_ENCODING_VALUES = 512;
constexpr std::size_t MAX_MODELS_PER_USER = 64;

bool is_number_list(const nlohmann::json& value) {
	if (!value.is_array()) {
		return false;
	}
	return std::ranges::all_of(value, [](const nlohmann::json& number) { return number.is_number(); });
}

// The model comes from a client, make sure it is something the
// authentication can work with before it is stored
void validate_new_model(const nlohmann::json& model) {
	if (!model.is_object()) {
		throw std::runtime_error("Model must be a dictionary");
	}
	if (!model.contains("time") || !model.at("time").is_number()) {
		throw std::runtime_error("Model has no valid time");
	}
	if (!model.contains("label") || !model.at("label").is_string()) {
		throw std::runtime_error("Model has no valid label");
	}
	if (model.at("label").get_ref<const std::string&>().size() > MAX_LABEL_BYTES) {
		throw std::runtime_error("Model label is too long");
	}
	if (!model.contains("data") || !model.at("data").is_array()) {
		throw std::runtime_error("Model has no valid data");
	}
	if (model.at("data").size() > MAX_ENCODINGS_PER_MODEL) {
		throw std::runtime_error("Model has too many encodings");
	}
	for (const auto& encoding : model.at("data")) {
		if (!is_number_list(encoding) || encoding.size() > MAX_ENCODING_VALUES) {
			throw std::runtime_error("Model has no valid data");
		}
	}
}

// std::nullopt if the user has no models
std::optional<nlohmann::json> load(const std::filesystem::path& path, const std::string& username) {
	const auto content = read_model_file(path, username);
	if (!content.has_value()) {
		return std::nullopt;
	}

	auto models = nlohmann::json::parse(*content, nullptr, false);
	if (models.is_discarded() || !models.is_array()) {
		throw std::runtime_error("Face model file is invalid");
	}
	for (const auto& model : models) {
		if (!model.is_object()) {
			throw std::runtime_error("Face model file is invalid");
		}
	}
	return models;
}

void save(const std::filesystem::path& path, const std::string& username, const nlohmann::json& models) {
	write_model_file(path, username, models.dump());
}

} // namespace

ModelStore::ModelStore(std::filesystem::path directory) : directory_(std::move(directory)) {
}

std::filesystem::path ModelStore::path_for(const std::string& username) const {
	// The name is only used after it was looked up in the user database, but
	// never trust that with a path
	if (username.empty() || username == "." || username == ".." || username.find_first_of("/\\") != std::string::npos ||
		username.find('\0') != std::string::npos) {
		throw std::runtime_error("Invalid user name");
	}
	return directory_ / (username + ".dat");
}

void ModelStore::add(const std::string& username, nlohmann::json model) const {
	validate_new_model(model);
	const auto path = path_for(username);

	std::error_code ec;
	if (!std::filesystem::exists(directory_, ec)) {
		std::filesystem::create_directories(directory_, ec);
		if (ec) {
			throw std::runtime_error("Could not create the models directory: " + ec.message());
		}
	}
	// Also for a directory of an older version that others could search
	chmod(directory_.c_str(), MODELS_DIR_MODE);

	auto models = load(path, username).value_or(nlohmann::json::array());
	if (models.size() >= MAX_MODELS_PER_USER) {
		throw std::runtime_error("Too many face models, please remove one first");
	}

	long long id = 0;
	if (!models.empty()) {
		const auto& last = models.back();
		if (!last.contains("id") || !last.at("id").is_number_integer()) {
			throw std::runtime_error("Existing face model file is invalid");
		}
		id = last.at("id").get<long long>() + 1;
	}
	model["id"] = id;

	models.push_back(std::move(model));
	save(path, username, models);
}

std::string ModelStore::list(const std::string& username) const {
	const auto models = load(path_for(username), username);

	auto result = nlohmann::json::array();
	if (models.has_value()) {
		for (const auto& model : *models) {
			if (!model.contains("time") || !model.contains("label") || !model.contains("id")) {
				throw std::runtime_error("Face model file is invalid");
			}
			result.push_back({{"time", model.at("time")}, {"label", model.at("label")}, {"id", model.at("id")}});
		}
	}
	return result.dump();
}

void ModelStore::remove(const std::string& username, const std::string& model_id) const {
	const auto path = path_for(username);
	const auto models = load(path, username);
	if (!models.has_value()) {
		throw std::runtime_error("No face model known for user " + username);
	}

	// The ids are compared as text, like the client sends them
	auto remaining = nlohmann::json::array();
	for (const auto& model : *models) {
		const auto id = model.find("id");
		const std::string text = id == model.end() ? "null" : (id->is_string() ? id->get<std::string>() : id->dump());
		if (text != model_id) {
			remaining.push_back(model);
		}
	}

	if (remaining.size() == models->size()) {
		throw std::runtime_error("No model with ID " + model_id + " exists for " + username);
	}

	if (remaining.empty()) {
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec) {
			throw std::runtime_error("Could not remove the face model file: " + ec.message());
		}
	} else {
		save(path, username, remaining);
	}
}

EncryptionReport ModelStore::encrypt_all() const {
	const auto key = load_models_key();
	if (!key.has_value()) {
		throw std::runtime_error("There is no " + std::string(MODELS_KEY_CREDENTIAL) + " credential, the models can not be encrypted");
	}

	EncryptionReport report;
	std::error_code ec;
	if (!std::filesystem::is_directory(directory_, ec)) {
		return report;
	}
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
		if (entry.path().extension() == ".dat") {
			files.push_back(entry.path());
		}
	}
	if (ec) {
		throw std::runtime_error("Could not list the models directory: " + ec.message());
	}
	std::ranges::sort(files);

	for (const auto& path : files) {
		try {
			const std::string username = path.stem().string();
			// Also checks the name
			static_cast<void>(path_for(username));

			// A link or a file of somebody else is refused here
			const auto content = read_trusted_file(path);
			if (!content.has_value()) {
				continue;
			}
			if (is_encrypted_model_file(*content)) {
				static_cast<void>(decrypt_model_file(*key, username, *content));
				++report.already_encrypted;
				continue;
			}
			// Do not encrypt what the daemons could not read
			const auto models = nlohmann::json::parse(*content, nullptr, false);
			if (models.is_discarded() || !models.is_array()) {
				throw std::runtime_error("the file is invalid");
			}

			write_model_file(path, username, *content);
			if (read_model_file(path, username) != *content) {
				throw std::runtime_error("the encrypted file does not read back");
			}
			++report.encrypted;
		} catch (const std::exception& exc) {
			report.failures.push_back(path.filename().string() + ": " + exc.what());
		}
	}
	return report;
}

std::optional<FaceMatch> ModelStore::compare(const std::string& username, Metric metric, const std::vector<double>& face) const {
	if (face.empty() || face.size() > MAX_ENCODING_VALUES) {
		throw std::runtime_error("Invalid face encoding");
	}
	const auto models = load(path_for(username), username);
	if (!models.has_value()) {
		return std::nullopt;
	}

	std::optional<FaceMatch> best;
	for (const auto& model : *models) {
		const auto data = model.find("data");
		if (data == model.end() || !data->is_array()) {
			continue;
		}
		// A model is used completely or not at all, models of the other backend have another size
		const bool matching =
			std::ranges::all_of(*data, [&](const nlohmann::json& encoding) { return is_number_list(encoding) && encoding.size() == face.size(); });
		if (!matching) {
			continue;
		}
		const std::string label = model.value("label", std::string());

		for (const auto& encoding : *data) {
			double value = 0;
			if (metric == Metric::Dot) {
				for (std::size_t k = 0; k < face.size(); ++k) {
					value += encoding[k].get<double>() * face[k];
				}
			} else {
				for (std::size_t k = 0; k < face.size(); ++k) {
					const double difference = encoding[k].get<double>() - face[k];
					value += difference * difference;
				}
				value = std::sqrt(value);
			}
			const bool better = !best.has_value() || (metric == Metric::Dot ? value > best->value : value < best->value);
			if (better) {
				best = FaceMatch{value, label};
			}
		}
	}
	return best;
}

KeyCheck ModelStore::verify_key(const std::string& key) const {
	KeyCheck check;
	std::error_code ec;
	if (!std::filesystem::is_directory(directory_, ec)) {
		return check;
	}
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
		if (entry.path().extension() == ".dat") {
			files.push_back(entry.path());
		}
	}
	if (ec) {
		throw std::runtime_error("Could not list the models directory: " + ec.message());
	}
	std::ranges::sort(files);

	for (const auto& path : files) {
		try {
			const auto content = read_trusted_file(path);
			if (!content.has_value() || !is_encrypted_model_file(*content)) {
				continue;
			}
			static_cast<void>(decrypt_model_file(key, path.stem().string(), *content));
			++check.matching;
		} catch (const std::exception& exc) {
			check.failures.push_back(path.filename().string() + ": " + exc.what());
		}
	}
	return check;
}

void ModelStore::clear(const std::string& username) const {
	const auto path = path_for(username);

	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec)) {
		throw std::runtime_error(username + " has no models or they have already been cleared");
	}
	std::filesystem::remove(path, ec);
	if (ec) {
		throw std::runtime_error("Could not remove the face model file: " + ec.message());
	}
}

} // namespace howdy

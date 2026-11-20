#include "model_store.hh"

#include <algorithm>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <sys/stat.h>

#include "trusted_file.hh"
#include "util.hh"

namespace howdy {

namespace {

// The directory can be searched by everybody, but not listed: the PAM module
// checks with stat() whether a user has a model before it asks the daemon, and
// it runs as whoever calls PAM, not always as root. With 0700 that check would
// fail for those and the face authentication would silently be skipped. The
// files in it are only accessible for root (MODEL_FILE_MODE).
constexpr mode_t MODELS_DIR_MODE = 0711;
constexpr mode_t MODEL_FILE_MODE = 0600;

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
std::optional<nlohmann::json> load(const std::filesystem::path& path) {
	const auto content = read_trusted_file(path);
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

void save(const std::filesystem::path& path, const nlohmann::json& models) {
	write_file_atomic(path, models.dump(), MODEL_FILE_MODE);
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
		chmod(directory_.c_str(), MODELS_DIR_MODE);
	}

	auto models = load(path).value_or(nlohmann::json::array());
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
	save(path, models);
}

std::string ModelStore::list(const std::string& username) const {
	const auto models = load(path_for(username));

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
	const auto models = load(path);
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
		save(path, remaining);
	}
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

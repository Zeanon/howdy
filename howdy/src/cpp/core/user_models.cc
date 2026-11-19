#include "user_models.hh"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "paths.hh"

namespace howdy {

std::optional<UserModels> load_user_models(const std::string& username, std::size_t encoding_size) {
	const auto path = std::filesystem::path(paths::USER_MODELS_DIR) / (username + ".dat");

	std::ifstream file(path);
	if (!file) {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			return std::nullopt;
		}
		throw std::runtime_error("Could not open face model file " + path.string());
	}

	const auto models = nlohmann::json::parse(file);
	if (!models.is_array()) {
		throw std::runtime_error("Face model file " + path.string() + " does not contain a list of models");
	}

	UserModels result;
	for (const auto& model : models) {
		if (!model.is_object() || !model.contains("data")) {
			continue;
		}
		const auto& data = model.at("data");

		bool all_matching = true;
		for (const auto& dat : data) {
			all_matching = all_matching && dat.is_array() && dat.size() == encoding_size;
		}
		if (!all_matching) {
			continue;
		}

		const std::string label = model.value("label", std::string());
		for (const auto& dat : data) {
			result.encodings.push_back(dat.get<Encoding>());
			result.labels.push_back(label);
		}
	}
	return result;
}

std::optional<std::vector<Encoding>> load_user_encodings(const std::string& username, std::size_t encoding_size) {
	auto models = load_user_models(username, encoding_size);
	if (!models.has_value()) {
		return std::nullopt;
	}
	return std::move(models->encodings);
}

} // namespace howdy

#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace howdy {

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

	void remove(const std::string& username, const std::string& model_id) const;

	// Delete all models of the user
	void clear(const std::string& username) const;

  private:
	[[nodiscard]] std::filesystem::path path_for(const std::string& username) const;

	std::filesystem::path directory_;
};

} // namespace howdy

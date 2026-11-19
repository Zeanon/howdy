#pragma once

#include <optional>
#include <string>
#include <vector>

#include "face_backend.hh"

namespace howdy {

struct UserModels {
	// Every stored encoding of the usable models
	std::vector<Encoding> encodings;
	// The label of the model each encoding belongs to
	std::vector<std::string> labels;
};

// Load all stored encodings of the user with the given size. Models whose
// encodings have a different size (dlib vs. OpenVINO) are skipped, like
// enroll_capture.flatten_models() does.
//
// Returns std::nullopt if the user has no model file. Throws on unreadable or
// malformed files.
std::optional<UserModels> load_user_models(const std::string& username, std::size_t encoding_size);

// Only the encodings, see load_user_models()
std::optional<std::vector<Encoding>> load_user_encodings(const std::string& username, std::size_t encoding_size);

} // namespace howdy

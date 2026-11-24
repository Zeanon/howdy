#pragma once

#include <cstddef>

#include "util.hh"

namespace howdy {

// From this many models on, adding another one gets a warning: `howdy add` and the windows give
// it, and the Python version of this is src/python/common/model_limits.py
inline constexpr std::size_t MANY_MODELS = 3;

// Whether a user with this many models gets the warning when adding another one
inline bool many_models(std::size_t count) {
	return count > MANY_MODELS;
}

// What the warning says
inline const char* many_models_warning() {
	return S("Each additional model slows down the face recognition engine slightly");
}

} // namespace howdy

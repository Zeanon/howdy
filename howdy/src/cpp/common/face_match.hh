#pragma once

#include <string>

namespace howdy {

// The best match of a face with the models of a user: the value of the comparison (a
// distance or a dot product, see Metric in model_store.hh) and the label of the model
struct FaceMatch {
	double value = 0;
	std::string label;
};

} // namespace howdy

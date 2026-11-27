#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>

namespace howdy {

// The time of a model, nothing if it has none that fits into 64 bits
inline std::optional<std::int64_t> get_model_time(const nlohmann::json& model) {
	const auto time = model.find("time");
	if (time == model.end() || !time->is_number()) {
		return std::nullopt;
	}

	const auto value = time->get<long double>();
	// 2^63 is exact even where long double is only a double. It is the first value
	// that does not fit, INT64_MAX may be rounded up to it.
	if (!std::isfinite(value) || value < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) || value >= std::ldexp(1.0L, 63)) {
		return std::nullopt;
	}

	return static_cast<std::int64_t>(value);
}

} // namespace howdy

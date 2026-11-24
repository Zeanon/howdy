#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <systemd/sd-bus.h>

#include "face_match.hh"

namespace howdy {

struct ModelInfo {
	long long id = 0;
	long long time = 0;
	std::string label;
};

// A call that was given up because the caller asked for it (see Cancelled)
class CallCancelled : public std::runtime_error {
  public:
	CallCancelled() : std::runtime_error("The call was cancelled") {
	}
};

// Asked while a call waits for its answer, true to give the call up
using Cancelled = std::function<bool()>;

// Client of the management daemon. All functions throw std::runtime_error
// with the message of the daemon on failure.
std::vector<ModelInfo> list_models(sd_bus* bus, const std::string& user);

// model_json: {"time": ..., "label": ..., "data": [[...], ...]}
void add_model(sd_bus* bus, const std::string& user, const std::string& model_json);

// The model is only removed or renamed if it still has the label (expected_label) and time
// (expected_time, seconds) the caller saw it with: the ids move up when a model is removed, an old
// list would hit another one.
void remove_model(sd_bus* bus, const std::string& user, std::int64_t id, const std::string& expected_label, std::int64_t expected_time);

// Give the model another label
void rename_model(
	sd_bus* bus, const std::string& user, std::int64_t id, const std::string& expected_label, std::int64_t expected_time, const std::string& label);

void clear_models(sd_bus* bus, const std::string& user);

// Let the daemon compare a face with the models of the user, which it does not give out.
// The metric is "euclidean" (dlib) or "dot" (OpenVINO), see ModelStore::compare().
// std::nullopt if the user has no model for the encoding. Only root may ask (it is not
// authorized through polkit), the daemon refuses everybody else. Given a cancelled function, the
// call is asked about it every 100 ms while it waits, and it ends with a CallCancelled when that
// says true (the answer is dropped, the daemon does not mind). Without one it waits for the answer
// or the timeout.
std::optional<FaceMatch> compare_face(
	sd_bus* bus, const std::string& user, const std::string& metric, const std::vector<double>& encoding, std::uint64_t timeout_usec,
	const Cancelled& cancelled = nullptr);

} // namespace howdy

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

// The daemon says that this user calls compare_face() too often: wait a moment and go on
class RateLimited : public std::runtime_error {
  public:
	using std::runtime_error::runtime_error;
};

// Asked while a call waits for its answer, true to give the call up
using Cancelled = std::function<bool()>;

// Client of the management daemon. All functions throw std::runtime_error
// with the message of the daemon on failure.
std::vector<ModelInfo> list_models(sd_bus* bus, const std::string& user);

// model_json: {"time": ..., "label": ..., "data": [[...], ...]}
void add_model(sd_bus* bus, const std::string& user, const std::string& model_json);

// The model is only removed or renamed if it still has the label the caller saw it with
// (expected_label) and time (expected_time, seconds as text): the ids move up when a model is
// removed, an old list would hit another one.
void remove_model(sd_bus* bus, const std::string& user, const std::string& id, const std::string& expected_label, const std::string& expected_time);

// Give the model another label
void rename_model(
	sd_bus* bus, const std::string& user, const std::string& id, const std::string& expected_label, const std::string& expected_time, const std::string& label);

void clear_models(sd_bus* bus, const std::string& user);

// Let the daemon compare a face with the models of the user, which it does not give out.
// The metric is "euclidean" (dlib) or "dot" (OpenVINO), see ModelStore::compare().
// std::nullopt if the user has no model for the encoding. The call is authorized
// through polkit on the first call of the connection only, and that call may wait for the
// password: pass POLKIT_TIMEOUT_USEC for it, a short timeout is enough for the others. Given a
// cancelled function, the call is asked about it every 100 ms while it waits, and it ends with a
// CallCancelled when that says true (the answer is dropped, the daemon does not mind). Without one
// it waits for the answer or the timeout.
std::optional<FaceMatch> compare_face(
	sd_bus* bus, const std::string& user, const std::string& metric, const std::vector<double>& encoding, std::uint64_t timeout_usec,
	const Cancelled& cancelled = nullptr);

// Extend the authorization of this connection for compare_face(): the daemon keeps it for a short
// time only after each call (a keep-alive like the one of the authentication daemon). It allows
// nothing itself, false if the connection was not authorized (any more), the next compare_face()
// then asks polkit again.
bool keep_alive(sd_bus* bus, std::uint64_t timeout_usec, const Cancelled& cancelled = nullptr);

} // namespace howdy

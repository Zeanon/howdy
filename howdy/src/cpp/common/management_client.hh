#pragma once

#include <string>
#include <vector>

#include <systemd/sd-bus.h>

namespace howdy {

struct ModelInfo {
	long long id = 0;
	long long time = 0;
	std::string label;
};

// Client of the management daemon. All functions throw std::runtime_error
// with the message of the daemon on failure.
std::vector<ModelInfo> list_models(sd_bus* bus, const std::string& user);

// model_json: {"time": ..., "label": ..., "data": [[...], ...]}
void add_model(sd_bus* bus, const std::string& user, const std::string& model_json);

void remove_model(sd_bus* bus, const std::string& user, const std::string& id);

void clear_models(sd_bus* bus, const std::string& user);

} // namespace howdy

#include "management_client.hh"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "paths.hh"
#include "polkit.hh"

namespace howdy {

namespace {

struct MessageDeleter {
	void operator()(sd_bus_message* message) const {
		sd_bus_message_unref(message);
	}
};
using MessagePtr = std::unique_ptr<sd_bus_message, MessageDeleter>;

// Call a method of the management interface with the given strings as
// arguments. The daemon may wait for the user to authenticate, so the timeout
// is long. Returns the reply, an error of the daemon becomes an exception.
MessagePtr call(sd_bus* bus, const char* member, const std::vector<const std::string*>& arguments) {
	sd_bus_message* raw = nullptr;
	int r = sd_bus_message_new_method_call(bus, &raw, paths::MANAGEMENT_BUS, paths::MANAGEMENT_PATH, paths::MANAGEMENT_INTERFACE, member);
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
	}
	const MessagePtr message(raw);

	for (const std::string* argument : arguments) {
		r = sd_bus_message_append(raw, "s", argument->c_str());
		if (r < 0) {
			throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
		}
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* raw_reply = nullptr;
	r = sd_bus_call(bus, raw, POLKIT_TIMEOUT_USEC, &error, &raw_reply);
	MessagePtr reply(raw_reply);
	if (r < 0) {
		const std::string text = error.message != nullptr ? error.message : std::strerror(-r);
		sd_bus_error_free(&error);
		throw std::runtime_error(text);
	}
	sd_bus_error_free(&error);
	return reply;
}

void expect_success(const MessagePtr& reply, const char* failure) {
	int success = 0;
	if (sd_bus_message_read(reply.get(), "b", &success) < 0 || success == 0) {
		throw std::runtime_error(failure);
	}
}

} // namespace

std::vector<ModelInfo> list_models(sd_bus* bus, const std::string& user) {
	const auto reply = call(bus, "List", {&user});

	const char* json = nullptr;
	if (sd_bus_message_read(reply.get(), "s", &json) < 0 || json == nullptr) {
		throw std::runtime_error("Invalid reply of the management daemon");
	}

	const auto models = nlohmann::json::parse(json, nullptr, false);
	if (models.is_discarded() || !models.is_array()) {
		throw std::runtime_error("Invalid reply of the management daemon");
	}

	std::vector<ModelInfo> result;
	for (const auto& model : models) {
		ModelInfo info;
		info.id = model.at("id").get<long long>();
		info.time = static_cast<long long>(model.at("time").get<double>());
		info.label = model.at("label").get<std::string>();
		result.push_back(std::move(info));
	}
	return result;
}

void add_model(sd_bus* bus, const std::string& user, const std::string& model_json) {
	expect_success(call(bus, "Add", {&user, &model_json}), "Failed to add face model");
}

void remove_model(sd_bus* bus, const std::string& user, const std::string& id) {
	expect_success(call(bus, "Remove", {&user, &id}), "Failed to remove model");
}

void clear_models(sd_bus* bus, const std::string& user) {
	expect_success(call(bus, "Clear", {&user}), "Failed to clear models");
}

std::optional<FaceMatch>
compare_face(sd_bus* bus, const std::string& user, const std::string& metric, const std::vector<double>& encoding, std::uint64_t timeout_usec) {
	sd_bus_message* raw = nullptr;
	int r = sd_bus_message_new_method_call(bus, &raw, paths::MANAGEMENT_BUS, paths::MANAGEMENT_PATH, paths::MANAGEMENT_INTERFACE, "Compare");
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
	}
	const MessagePtr message(raw);

	r = sd_bus_message_append(raw, "ss", user.c_str(), metric.c_str());
	if (r >= 0) {
		r = sd_bus_message_append_array(raw, 'd', encoding.data(), encoding.size() * sizeof(double));
	}
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* raw_reply = nullptr;
	r = sd_bus_call(bus, raw, timeout_usec, &error, &raw_reply);
	const MessagePtr reply(raw_reply);
	if (r < 0) {
		const std::string text = error.message != nullptr ? error.message : std::strerror(-r);
		sd_bus_error_free(&error);
		throw std::runtime_error(text);
	}
	sd_bus_error_free(&error);

	int found = 0;
	double value = 0;
	const char* label = nullptr;
	if (sd_bus_message_read(reply.get(), "bds", &found, &value, &label) < 0 || label == nullptr) {
		throw std::runtime_error("Invalid reply of the management daemon");
	}
	if (found == 0) {
		return std::nullopt;
	}
	return FaceMatch{value, label};
}

bool keep_alive(sd_bus* bus, std::uint64_t timeout_usec) {
	sd_bus_message* raw = nullptr;
	int r = sd_bus_message_new_method_call(bus, &raw, paths::MANAGEMENT_BUS, paths::MANAGEMENT_PATH, paths::MANAGEMENT_INTERFACE, "KeepAlive");
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
	}
	const MessagePtr message(raw);

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* raw_reply = nullptr;
	r = sd_bus_call(bus, raw, timeout_usec, &error, &raw_reply);
	const MessagePtr reply(raw_reply);
	if (r < 0) {
		const std::string text = error.message != nullptr ? error.message : std::strerror(-r);
		sd_bus_error_free(&error);
		throw std::runtime_error(text);
	}
	sd_bus_error_free(&error);

	int kept = 0;
	if (sd_bus_message_read(reply.get(), "b", &kept) < 0) {
		throw std::runtime_error("Invalid reply of the management daemon");
	}
	return kept != 0;
}

} // namespace howdy

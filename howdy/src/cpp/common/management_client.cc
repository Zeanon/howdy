#include "management_client.hh"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "model_time.hh"
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

// Call a method of the management interface, `append` adds the arguments to the call. The daemon
// may wait for the user to authenticate, so the timeout is long. Returns the reply, an error of
// the daemon becomes an exception.
MessagePtr call(sd_bus* bus, const char* member, const std::function<int(sd_bus_message*)>& append) {
	sd_bus_message* raw = nullptr;
	int r = sd_bus_message_new_method_call(bus, &raw, paths::MANAGEMENT_BUS, paths::MANAGEMENT_PATH, paths::MANAGEMENT_INTERFACE, member);
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
	}
	const MessagePtr message(raw);

	r = append(raw);
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to create the call: ") + std::strerror(-r));
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

// Call a method with the given strings as arguments
MessagePtr call(sd_bus* bus, const char* member, const std::vector<const std::string*>& arguments) {
	return call(bus, member, [&](sd_bus_message* message) {
		for (const std::string* argument : arguments) {
			const int r = sd_bus_message_append(message, "s", argument->c_str());
			if (r < 0) {
				return r;
			}
		}
		return 0;
	});
}

// What the callback of an asynchronous call leaves
struct AsyncResult {
	bool done = false;
	MessagePtr reply;
	std::string error;
	std::string error_name;
};

int on_async_reply(sd_bus_message* reply, void* userdata, sd_bus_error* /*ret_error*/) {
	auto* result = static_cast<AsyncResult*>(userdata);
	if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
		const sd_bus_error* error = sd_bus_message_get_error(reply);
		result->error = error != nullptr && error->message != nullptr ? error->message : "The call failed";
		result->error_name = error != nullptr && error->name != nullptr ? error->name : "";
	} else {
		result->reply.reset(sd_bus_message_ref(reply));
	}
	result->done = true;
	return 0;
}

struct SlotDeleter {
	void operator()(sd_bus_slot* slot) const {
		sd_bus_slot_unref(slot);
	}
};

// Send a call and wait for the answer. With a cancelled function the call is asynchronous and
// it is asked about every 100 ms: dropping the slot drops the answer, so a caller that has to stop
// is not held up by a call that waits long (the daemon asking polkit for a password).
MessagePtr send(sd_bus* bus, sd_bus_message* message, std::uint64_t timeout_usec, const Cancelled& cancelled) {
	if (!cancelled) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message* raw_reply = nullptr;
		const int r = sd_bus_call(bus, message, timeout_usec, &error, &raw_reply);
		MessagePtr reply(raw_reply);
		if (r < 0) {
			const std::string text = error.message != nullptr ? error.message : std::strerror(-r);
			sd_bus_error_free(&error);
			throw std::runtime_error(text);
		}
		sd_bus_error_free(&error);
		return reply;
	}

	AsyncResult result;
	sd_bus_slot* raw_slot = nullptr;
	int r = sd_bus_call_async(bus, &raw_slot, message, on_async_reply, &result, timeout_usec);
	if (r < 0) {
		throw std::runtime_error(std::string("Failed to send the call: ") + std::strerror(-r));
	}
	const std::unique_ptr<sd_bus_slot, SlotDeleter> slot(raw_slot);

	constexpr std::uint64_t CANCEL_CHECK_USEC = 100000;
	while (!result.done) {
		if (cancelled()) {
			throw CallCancelled();
		}
		r = sd_bus_process(bus, nullptr);
		if (r < 0) {
			throw std::runtime_error(std::string("Failed to process the bus: ") + std::strerror(-r));
		}
		if (r == 0 && !result.done) {
			r = sd_bus_wait(bus, CANCEL_CHECK_USEC);
			if (r < 0) {
				throw std::runtime_error(std::string("Failed to wait for the bus: ") + std::strerror(-r));
			}
		}
	}
	if (!result.error.empty()) {
		throw std::runtime_error(result.error);
	}
	return std::move(result.reply);
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
		const auto time = get_model_time(model);
		if (!time.has_value()) {
			throw std::runtime_error("Invalid model time in reply of the management daemon");
		}
		info.time = *time;
		info.label = model.at("label").get<std::string>();
		result.push_back(std::move(info));
	}
	return result;
}

void add_model(sd_bus* bus, const std::string& user, const std::string& model_json) {
	expect_success(call(bus, "Add", {&user, &model_json}), "Failed to add face model");
}

void remove_model(sd_bus* bus, const std::string& user, std::int64_t id, const std::string& expected_label, std::int64_t expected_time) {
	expect_success(
		call(
			bus, "Remove",
			[&](sd_bus_message* message) { return sd_bus_message_append(message, "sxsx", user.c_str(), id, expected_label.c_str(), expected_time); }),
		"Failed to remove model");
}

void rename_model(
	sd_bus* bus, const std::string& user, std::int64_t id, const std::string& expected_label, std::int64_t expected_time, const std::string& label) {
	expect_success(
		call(
			bus, "Rename",
			[&](sd_bus_message* message) {
				return sd_bus_message_append(message, "sxsxs", user.c_str(), id, expected_label.c_str(), expected_time, label.c_str());
			}),
		"Failed to rename model");
}

void clear_models(sd_bus* bus, const std::string& user) {
	expect_success(call(bus, "Clear", {&user}), "Failed to clear models");
}

std::optional<FaceMatch> compare_face(
	sd_bus* bus, const std::string& user, const std::string& metric, const std::vector<double>& encoding, std::uint64_t timeout_usec,
	const Cancelled& cancelled) {
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

	const MessagePtr reply = send(bus, raw, timeout_usec, cancelled);

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

} // namespace howdy

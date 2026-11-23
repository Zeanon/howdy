#include "gdbus_client.hh"

#include <memory>

#include "paths.hh"

namespace gtkui {

namespace {

// The daemon answers a failed authorization with this name
constexpr const char* NOT_AUTHORIZED_ERROR = howdy::paths::MANAGEMENT_NOT_AUTHORIZED_ERROR;

// The time the user gets to answer the password prompt of polkit
constexpr gint TIMEOUT_MSEC = 300 * 1000;

struct Pending {
	ManagementClient::Callback callback;
};

void on_reply(GObject* source, GAsyncResult* async_result, gpointer data) {
	const std::unique_ptr<Pending> pending(static_cast<Pending*>(data));

	ManagementResult result;
	GError* error = nullptr;
	GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), async_result, &error);

	if (reply != nullptr) {
		result.ok = true;
		// List answers with its JSON, the others with a success flag
		if (g_variant_is_of_type(reply, G_VARIANT_TYPE("(s)"))) {
			const gchar* json = nullptr;
			g_variant_get(reply, "(&s)", &json);
			result.reply = json;
		} else if (g_variant_is_of_type(reply, G_VARIANT_TYPE("(b)"))) {
			gboolean success = FALSE;
			g_variant_get(reply, "(b)", &success);
			result.ok = success;
			if (!result.ok) {
				result.error = "The management daemon did not accept the request";
			}
		}
		g_variant_unref(reply);
	} else {
		if (g_dbus_error_is_remote_error(error)) {
			gchar* name = g_dbus_error_get_remote_error(error);
			result.not_authorized = name != nullptr && std::string(name) == NOT_AUTHORIZED_ERROR;
			g_free(name);
		}
		// Without the name of the D-Bus error in front of it
		g_dbus_error_strip_remote_error(error);
		result.error = error->message;
		g_clear_error(&error);
	}

	pending->callback(result);
}

} // namespace

ManagementClient::~ManagementClient() {
	if (connection_ != nullptr) {
		g_object_unref(connection_);
	}
}

void ManagementClient::call(const char* method, GVariant* parameters, Callback callback) {
	if (connection_ == nullptr) {
		GError* error = nullptr;
		connection_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
		if (connection_ == nullptr) {
			g_variant_unref(g_variant_ref_sink(parameters));

			auto* failed = new std::pair<Callback, std::string>(
				std::move(callback), error != nullptr ? error->message : "No connection to the system bus"); // NOLINT(cppcoreguidelines-owning-memory)
			g_clear_error(&error);
			g_idle_add(
				+[](gpointer data) -> gboolean {
					const std::unique_ptr<std::pair<Callback, std::string>> failed(static_cast<std::pair<Callback, std::string>*>(data));
					ManagementResult result;
					result.error = failed->second;
					failed->first(result);
					return G_SOURCE_REMOVE;
				},
				failed);
			return;
		}
	}

	auto* pending = new Pending{std::move(callback)}; // NOLINT(cppcoreguidelines-owning-memory)
	g_dbus_connection_call(
		connection_, howdy::paths::MANAGEMENT_BUS, howdy::paths::MANAGEMENT_PATH, howdy::paths::MANAGEMENT_INTERFACE, method, parameters, nullptr,
		G_DBUS_CALL_FLAGS_NONE, TIMEOUT_MSEC, nullptr, on_reply, pending);
}

void ManagementClient::list(const std::string& user, Callback callback) {
	call("List", g_variant_new("(s)", user.c_str()), std::move(callback));
}

void ManagementClient::add(const std::string& user, const std::string& model_json, Callback callback) {
	call("Add", g_variant_new("(ss)", user.c_str(), model_json.c_str()), std::move(callback));
}

void ManagementClient::remove(
	const std::string& user, const std::string& id, const std::string& expected_label, const std::string& expected_time, Callback callback) {
	call("Remove", g_variant_new("(ssss)", user.c_str(), id.c_str(), expected_label.c_str(), expected_time.c_str()), std::move(callback));
}

void ManagementClient::rename(
	const std::string& user, const std::string& id, const std::string& expected_label, const std::string& expected_time, const std::string& label,
	Callback callback) {
	call("Rename", g_variant_new("(sssss)", user.c_str(), id.c_str(), expected_label.c_str(), expected_time.c_str(), label.c_str()), std::move(callback));
}

} // namespace gtkui

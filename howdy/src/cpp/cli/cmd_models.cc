// howdy list, remove and clear: talk to the management daemon

#include <algorithm>
#include <array>
#include <ctime>
#include <iostream>

#include "cli.hh"
#include "management_client.hh"
#include "util.hh"

namespace howdy::cli {

namespace {

std::string format_time(long long timestamp) {
	const auto seconds = static_cast<std::time_t>(timestamp);
	std::tm local{};
	localtime_r(&seconds, &local);
	std::array<char, 32> buffer{};
	std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local);
	return buffer.data();
}

} // namespace

int run_list(const Options& options) {
	const BusPtr bus = open_system_bus();
	authorize(bus.get(), options);

	std::vector<ModelInfo> models;
	try {
		models = list_models(bus.get(), options.user);
	} catch (const std::exception& exc) {
		throw CliError(std::string(S("Failed to retrieve face models: ")) + exc.what());
	}

	if (models.empty()) {
		if (!options.plain) {
			std::cout << S("No face model known for the user ") << options.user << S(", please run:") << "\n\n\thowdy add\n\n";
		}
		return 1;
	}

	// Print a header if we're not in plain mode
	if (!options.plain) {
		std::cout << S("Known face models for ") << options.user << ":\n";
		std::cout << "\n\033[1;29m" << S("ID  Date                 Label") << "\033[0m\n";
	}

	for (const auto& model : models) {
		const std::string id = std::to_string(model.id);
		std::cout << id;

		// Add comma for machine reading, padding spaces for a nice layout otherwise
		if (options.plain) {
			std::cout << ",";
		} else if (id.size() < 4) {
			std::cout << std::string(4 - id.size(), ' ');
		}

		// The time as ISO in the local timezone
		std::cout << format_time(model.time) << (options.plain ? "," : "  ") << model.label << "\n";
	}

	// Add a closing enter
	std::cout << '\n';
	return 0;
}

int run_remove(const Options& options) {
	const BusPtr bus = open_system_bus();
	authorize(bus.get(), options);

	// Check if enough arguments have been passed
	if (options.arguments.empty()) {
		std::string reason = S("Please add the ID of the model you want to remove as an argument\n");
		reason += S("For example:\n");
		reason += "\thowdy remove 0\n";
		reason += S("You can find the IDs by running:\n");
		reason += "\thowdy list";
		throw CliError(reason);
	}

	std::vector<ModelInfo> models;
	try {
		models = list_models(bus.get(), options.user);
	} catch (const std::exception& exc) {
		throw CliError(std::string(S("Failed to retrieve face models: ")) + exc.what());
	}

	// Get the ID from the cli arguments and look for the model
	const std::string& id = options.arguments[0];
	const ModelInfo* found = nullptr;
	for (const auto& model : models) {
		if (std::to_string(model.id) == id) {
			found = &model;
			break;
		}
	}

	if (found == nullptr) {
		throw CliError(std::string(S("No model with ID ")) + id + " exists for " + options.user);
	}

	// Only ask the user if there's no -y flag
	if (!options.yes) {
		std::cout << S("This will remove the model called \"") << found->label << "\" for " << options.user << "\n";
		if (!confirm(S("Do you want to continue [y/N]: "))) {
			throw CliError(std::string("\n") + S("Interpreting as a \"NO\", aborting"));
		}
		// Add a padding empty line
		std::cout << '\n';
	}

	try {
		remove_model(bus.get(), options.user, found->id, found->label, found->time);
	} catch (const std::exception& exc) {
		throw CliError(std::string("Failed to remove face model: ") + exc.what());
	}

	if (models.size() == 1) {
		say(options, S("Removed last model, howdy disabled for user"));
	} else {
		say(options, std::string(S("Removed model ")) + id);
	}
	return 0;
}

int run_rename(const Options& options) {
	const BusPtr bus = open_system_bus();
	authorize(bus.get(), options);

	// Check if enough arguments have been passed
	if (options.arguments.empty()) {
		std::string reason = S("Please add the ID of the model you want to rename as an argument\n");
		reason += S("For example:\n");
		reason += "\thowdy rename 0\n";
		reason += "\thowdy rename 0 \"Living room\"\n";
		reason += S("You can find the IDs by running:\n");
		reason += "\thowdy list";
		throw CliError(reason);
	}

	std::vector<ModelInfo> models;
	try {
		models = list_models(bus.get(), options.user);
	} catch (const std::exception& exc) {
		throw CliError(std::string(S("Failed to retrieve face models: ")) + exc.what());
	}

	// Get the ID from the cli arguments and look for the model
	const std::string& id = options.arguments[0];
	const ModelInfo* found = nullptr;
	for (const auto& model : models) {
		if (std::to_string(model.id) == id) {
			found = &model;
			break;
		}
	}

	if (found == nullptr) {
		throw CliError(std::string(S("No model with ID ")) + id + " exists for " + options.user);
	}

	// Get the new name from the cli arguments if provided, otherwise ask for it
	std::string label;
	if (options.arguments.size() > 1) {
		label = options.arguments[1];
	} else {
		// Nobody can answer with -y, or without a TTY / with a closed stdin
		if (options.yes || isatty(STDIN_FILENO) == 0) {
			throw CliError(S("Please add the new name of the model as an argument"));
		}
		label = ask(std::string(S("Enter a new label for this model [")) + found->label + "]: ");
		if (label.empty()) {
			label = found->label;
		} else {
			// The same limit as when adding a model
			label = truncate_characters(label, 24);
		}
	}

	// Remove illegal characters
	if (label.find(',') != std::string::npos) {
		say(options, S("NOTICE: Removing illegal character \",\" from model name"));
		std::erase(label, ',');
	}
	if (label.empty()) {
		throw CliError(S("The new name of the model is empty"));
	}

	try {
		rename_model(bus.get(), options.user, found->id, found->label, found->time, label);
	} catch (const std::exception& exc) {
		throw CliError(std::string("Failed to rename face model: ") + exc.what());
	}

	say(options, std::string(S("Renamed model ")) + id);
	return 0;
}

int run_clear(const Options& options) {
	const BusPtr bus = open_system_bus();
	authorize(bus.get(), options);

	if (!options.yes) {
		std::cout << S("This will clear all models for ") << options.user << "\n";
		if (!confirm(S("Do you want to continue [y/N]: "))) {
			throw CliError(std::string("\n") + S("Interpreting as a \"NO\", aborting"));
		}
	}

	try {
		clear_models(bus.get(), options.user);
	} catch (const std::exception& exc) {
		throw CliError(std::string(S("Failed to clear models: ")) + exc.what());
	}

	std::cout << "\n" << S("Models cleared") << '\n';
	return 0;
}

} // namespace howdy::cli

#include "model_enroller.hh"

#include <ctime>
#include <filesystem>
#include <thread>

#include <nlohmann/json.hpp>

#include "config.hh"
#include "face_backend.hh"
#include "face_setup.hh"
#include "paths.hh"
#include "video_capture.hh"

namespace gtkui {

// What the thread prepares for the capture
struct ModelEnroller::Job {
	ModelEnroller* enroller;
	std::shared_ptr<bool> alive;
	GtkWindow* parent;
	std::string label;
	Done done;
	std::function<void()> ready;

	// The video capture keeps a reference to the config
	std::unique_ptr<howdy::Config> config;
	std::unique_ptr<howdy::FaceBackend> backend;
	std::unique_ptr<howdy::VideoCapture> camera;
	double dark_threshold = 60;
	std::string error;
};

ModelEnroller::ModelEnroller(ManagementClient& client, std::string user) : client_(client), user_(std::move(user)) {
}

ModelEnroller::~ModelEnroller() {
	*alive_ = false;
}

void ModelEnroller::enroll(GtkWindow* parent, const std::string& label, Done done, std::function<void()> ready) {
	auto* job = new Job{this, alive_, parent, label, std::move(done), std::move(ready), {}, {}, {}, 60, ""}; // NOLINT(cppcoreguidelines-owning-memory)
	const std::string user = user_;

	// Loading the models and opening the camera takes seconds
	std::thread([job, user] {
		try {
			job->config = std::make_unique<howdy::Config>((std::filesystem::path(howdy::paths::CONFIG_DIR) / "config.ini").string());
			job->dark_threshold = job->config->get_float("video", "dark_threshold", 60);
			job->backend = howdy::create_face_backend(*job->config, user);
			job->camera = std::make_unique<howdy::VideoCapture>(*job->config);
		} catch (const std::exception& exc) {
			job->error = exc.what();
		}
		g_idle_add(on_prepared, job);
	}).detach();
}

gboolean ModelEnroller::on_prepared(gpointer data) {
	std::unique_ptr<Job> job(static_cast<Job*>(data));
	if (!*job->alive) {
		return G_SOURCE_REMOVE;
	}
	ModelEnroller* self = job->enroller;

	const auto finish = [&job](ModelEnroller::Outcome outcome, const std::string& error = "") {
		Result result;
		result.outcome = outcome;
		result.error = error;
		job->done(result);
	};

	if (!job->error.empty()) {
		finish(Outcome::Failed, job->error);
		return G_SOURCE_REMOVE;
	}

	if (job->ready) {
		job->ready();
	}

	// The dialog runs its own main loop and returns when it is over
	std::vector<howdy::Encoding> samples;
	try {
		howdy::EnrollSettings settings{*job->backend, *job->camera, job->dark_threshold, [](const std::string&) {}, self->timing_};
		settings.parent = job->parent;
		settings.rotate = static_cast<int>(job->config->get_int("video", "rotate", 0));
		samples = howdy::capture_guided_samples(settings);
	} catch (const howdy::EnrollCancelled&) {
		finish(Outcome::Cancelled);
		return G_SOURCE_REMOVE;
	} catch (const std::exception& exc) {
		finish(Outcome::Failed, exc.what());
		return G_SOURCE_REMOVE;
	}

	// The window may have been closed while the dialog was shown
	if (!*job->alive) {
		return G_SOURCE_REMOVE;
	}

	// The camera is not needed any more
	job->camera.reset();

	if (samples.empty()) {
		finish(Outcome::Failed, "No samples captured, aborting");
		return G_SOURCE_REMOVE;
	}

	const nlohmann::json model = {
		{"time", static_cast<long long>(std::time(nullptr))},
		{"label", job->label},
		{"data", samples},
	};

	// Saved over the connection that is kept open
	auto shared_job = std::shared_ptr<Job>(std::move(job));
	self->client_.add(self->user_, model.dump(), [shared_job](const ManagementResult& saved) {
		if (!*shared_job->alive) {
			return;
		}
		Result result;
		result.outcome = saved.ok ? Outcome::Saved : Outcome::Failed;
		result.error = saved.error;
		shared_job->done(result);
	});
	return G_SOURCE_REMOVE;
}

} // namespace gtkui

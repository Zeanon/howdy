#pragma once

#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

#include "enroll_dialog.hh"
#include "gdbus_client.hh"

namespace gtkui {

// Adds a face model: loads the face recognition and opens the camera, guides
// the user through the capture in a dialog and saves the model with the
// management daemon.
//
// Everything happens in this program, over the connection of the client: for
// polkit the model is saved by the same process as everything else that the
// window does, so the password is asked for once.
class ModelEnroller {
  public:
	enum class Outcome {
		Saved,
		// The user closed the dialog
		Cancelled,
		Failed,
	};

	struct Result {
		Outcome outcome = Outcome::Failed;
		// What went wrong, for the user
		std::string error;
	};

	using Done = std::function<void(const Result&)>;

	ModelEnroller(ManagementClient& client, std::string user);
	~ModelEnroller();

	ModelEnroller(const ModelEnroller&) = delete;
	ModelEnroller& operator=(const ModelEnroller&) = delete;
	ModelEnroller(ModelEnroller&&) = delete;
	ModelEnroller& operator=(ModelEnroller&&) = delete;

	void set_timing(howdy::EnrollTiming timing) {
		timing_ = timing;
	}

	// Capture and save a model with the label. The dialog belongs to the parent
	// window. Loading the models takes a while and does not block the user
	// interface, `ready` is called when it is done and the dialog is about to be
	// shown. `done` is called when everything is over, in the main loop and not
	// from within this function.
	void enroll(GtkWindow* parent, const std::string& label, Done done, std::function<void()> ready = {});

  private:
	struct Job;
	static gboolean on_prepared(gpointer data);

	ManagementClient& client_;
	std::string user_;
	howdy::EnrollTiming timing_;
	// Work in the background must not touch a destroyed enroller
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace gtkui

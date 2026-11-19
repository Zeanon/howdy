#pragma once

#include <functional>
#include <memory>
#include <string>

#include "config.hh"
#include "face_backend.hh"

namespace howdy {

// Load the face detection and encoding that the config asks for (dlib or
// OpenVINO), for adding a model. This takes a while and can run in a thread.
//
// It runs as a regular user, which can not write to the OpenVINO cache of the
// system: the compiled models are cached in the home of the user.
//
// Throws std::runtime_error with a message for the user, which says what to do
// if the data files are missing. `say` gets informational messages (may be empty).
std::unique_ptr<FaceBackend> create_face_backend(const Config& config, const std::string& user, const std::function<void(const std::string&)>& say = {});

} // namespace howdy

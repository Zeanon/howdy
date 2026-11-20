#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace howdy {

// Read a file that decides who may authenticate, so nobody but root and the
// user of the daemon may be able to change it. The file and its directory must
// belong to one of them and must not be writable by the group or others, and
// the file must be a regular file and not a link. The checks run on the opened
// file, so it can not be swapped in between.
// Returns std::nullopt if the file (or its directory) does not exist, throws
// std::runtime_error if it is not trusted or can not be read.
std::optional<std::string> read_trusted_file(const std::filesystem::path& path);

} // namespace howdy

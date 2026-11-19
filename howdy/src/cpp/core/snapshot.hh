#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy {

// Put the frames next to each other, add the lines of text and the Howdy
// logo below them and save the image to the snapshot directory (owner only).
// Returns the file name, empty if there are no frames. Throws
// std::runtime_error if the file can not be written.
std::filesystem::path generate_snapshot(const std::vector<cv::Mat>& frames, const std::vector<std::string>& text_lines);

} // namespace howdy

#pragma once

#include <chrono>
#include <cstddef>
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

// How long and how many snapshots are kept, zero means no limit
struct SnapshotRetention {
	std::chrono::seconds max_age{7 * 24 * 3600};
	std::size_t max_count = 200;
};

// Delete the snapshots that are older than the retention allows and, of the
// rest, the oldest ones beyond the maximum count. Only regular .jpg files of the
// snapshot directory are touched, symbolic links are not followed. Returns the
// number of deleted files, zero if the directory does not exist. Throws
// std::runtime_error if the directory can not be read.
std::size_t prune_snapshots(const SnapshotRetention& retention);

} // namespace howdy

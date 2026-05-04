// compute/image_io.hpp
//
// Thin OpenCV wrapper for PNG artifacts plus raw interactive frame transport.

#pragma once

#include <opencv2/core.hpp>

#include <string>

namespace fsd::compute {

// Write a BGR CV_8UC3 Mat to path. Creates parent dirs if needed.
// Uses PNG compression level 3 (fast, moderate size).
// Returns the written file path.
std::string write_png(const std::string& path, const cv::Mat& bgr);

// Read a PNG into a BGR CV_8UC3 Mat. Throws std::runtime_error on failure.
cv::Mat read_png(const std::string& path);

// Convert a BGR CV_8UC3 Mat into tightly-packed RGBA8 bytes for direct canvas
// upload. This avoids PNG encode/decode and disk I/O for interactive frames.
std::string encode_rgba8(const cv::Mat& bgr);

} // namespace fsd::compute

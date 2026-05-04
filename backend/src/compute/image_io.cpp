// compute/image_io.cpp

#include "image_io.hpp"

#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fsd::compute {

std::string write_png(const std::string& path, const cv::Mat& bgr) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::runtime_error("write_png: expected non-empty CV_8UC3 Mat");
    }
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
    const std::vector<int> params = {
        cv::IMWRITE_PNG_COMPRESSION, 3,
    };
    const std::filesystem::path tmp = p.parent_path() / (p.filename().string() + ".tmp.png");
    if (!cv::imwrite(tmp.string(), bgr, params)) {
        throw std::runtime_error("write_png: imwrite failed for " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(p, ec);
        ec.clear();
        std::filesystem::rename(tmp, p, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("write_png: rename failed for " + path);
    }
    return path;
}

cv::Mat read_png(const std::string& path) {
    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
    if (img.empty()) {
        throw std::runtime_error("read_png: imread failed for " + path);
    }
    return img;
}

std::string encode_rgba8(const cv::Mat& bgr) {
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        throw std::runtime_error("encode_rgba8: expected non-empty CV_8UC3 Mat");
    }
    const int width = bgr.cols;
    const int height = bgr.rows;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::string rgba(pixelCount * 4, '\0');
    auto* dst = reinterpret_cast<unsigned char*>(rgba.data());

    for (int y = 0; y < height; ++y) {
        const unsigned char* src = bgr.ptr<unsigned char>(y);
        unsigned char* row = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            row[0] = src[2];
            row[1] = src[1];
            row[2] = src[0];
            row[3] = 255;
            src += 3;
            row += 4;
        }
    }
    return rgba;
}

} // namespace fsd::compute

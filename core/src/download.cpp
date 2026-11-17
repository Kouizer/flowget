#include <flowget/download.h>

#include "download_util.h"

#include <algorithm>
#include <system_error>

namespace flowget {

double DownloadProgress::fraction() const {
    if (total_bytes <= 0) {
        return 0.0;
    }
    const auto written = static_cast<double>(bytes_written);
    const auto total = static_cast<double>(total_bytes);
    return std::clamp(written / total, 0.0, 1.0);
}

namespace detail {
namespace {

std::string utf8_string(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string{value.begin(), value.end()};
}

} // namespace

std::filesystem::path path_from_utf8(const std::string& utf8) noexcept {
    try {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    } catch (...) {
        // Windows throws when the bytes are not valid UTF-8; POSIX treats them as opaque, so the
        // conversion cannot fail there.
        return {};
    }
}

bool ensure_parent_directory(const std::string& utf8_path, std::string& error) {
    const std::filesystem::path path = path_from_utf8(utf8_path);
    if (path.empty()) {
        error = "invalid UTF-8 output path";
        return false;
    }
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        error =
            "cannot create directory " + utf8_string(parent) + ": " + filesystem_error.message();
        return false;
    }
    return true;
}

bool remove_file(const std::string& utf8_path, std::string& error) {
    const std::filesystem::path path = path_from_utf8(utf8_path);
    if (path.empty()) {
        error = "invalid UTF-8 output path";
        return false;
    }
    std::error_code filesystem_error;
    const bool removed = std::filesystem::remove(path, filesystem_error);
    static_cast<void>(removed);
    if (filesystem_error) {
        error = "cannot remove " + utf8_path + ": " + filesystem_error.message();
        return false;
    }
    return true;
}

} // namespace detail

} // namespace flowget

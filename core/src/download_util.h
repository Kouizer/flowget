#ifndef FLOWGET_DOWNLOAD_UTIL_H
#define FLOWGET_DOWNLOAD_UTIL_H

#include <filesystem>
#include <string>

namespace flowget::detail {

// UTF-8 -> std::filesystem::path. On Windows a path built from a narrow std::string uses the ANSI
// code page, so every path coming from DownloadOptions goes through this helper instead.
// Returns an empty path when the input is not valid UTF-8.
std::filesystem::path path_from_utf8(const std::string& utf8) noexcept;

// Creates the parent directory of `utf8_path` when it is missing. Returns false and fills `error`
// on failure.
bool ensure_parent_directory(const std::string& utf8_path, std::string& error);

// Deletes `utf8_path` when it exists. A missing file is success. Returns false and fills `error`
// when the file exists but cannot be removed.
bool remove_file(const std::string& utf8_path, std::string& error);

} // namespace flowget::detail

#endif /* FLOWGET_DOWNLOAD_UTIL_H */

#include "file_replace.h"

#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "download_util.h"
#include "utf8_path.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

namespace flowget::detail {
namespace {

constexpr std::string_view kTempSuffix = ".tmp";

// Writes the whole buffer and makes it durable. The temporary name is reused across checkpoints, so
// an abandoned attempt is truncated instead of appended to.
bool write_bytes(const std::string& path, std::string_view contents, std::string& error) {
    const std::unique_ptr<IStorage> storage = make_platform_storage();
    const StorageResult opened = storage->open(path, OpenMode::read_write_truncate);
    if (!opened.ok()) {
        error = "cannot create \"" + path + "\": " + opened.message;
        return false;
    }

    if (!contents.empty()) {
        const std::span<const char> text{contents.data(), contents.size()};
        const std::span<const std::byte> bytes = std::as_bytes(text);
        std::size_t written = 0;
        const StorageResult result = storage->write_at(0, bytes.data(), bytes.size(), written);
        if (!result.ok() || written != bytes.size()) {
            error = "cannot write \"" + path +
                    "\": " + (result.message.empty() ? "short write" : result.message);
            return false;
        }
    }

    // fsync before the rename: a sidecar that claims bytes which are still in the page cache would
    // survive a crash without the data it points at (tasks/todo.md S5).
    const StorageResult flushed = storage->flush();
    if (!flushed.ok()) {
        error = "cannot flush \"" + path + "\": " + flushed.message;
        return false;
    }
    const StorageResult closed = storage->close();
    if (!closed.ok()) {
        error = "cannot close \"" + path + "\": " + closed.message;
        return false;
    }
    return true;
}

} // namespace

bool replace_file(const std::string& temp_path, const std::string& final_path, std::string& error) {
    if (temp_path.empty() || final_path.empty()) {
        error = "cannot replace a file: the path is empty";
        return false;
    }
    if (temp_path == final_path) {
        error = "cannot replace a file with itself";
        return false;
    }

#ifdef _WIN32
    std::wstring wide_temp;
    std::wstring wide_final;
    if (!utf8_to_wide(temp_path, wide_temp) || !utf8_to_wide(final_path, wide_final)) {
        error = "cannot replace a file: the path is not valid UTF-8";
        return false;
    }
    if (::MoveFileExW(wide_temp.c_str(),
                      wide_final.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "cannot replace \"" + final_path + "\": " + win32_error_text(::GetLastError());
        return false;
    }
#else
    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        error = "cannot replace \"" + final_path + "\": " + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

bool write_file_atomically(const std::string& final_path,
                           std::string_view contents,
                           std::string& error) {
    if (final_path.empty()) {
        error = "cannot write a file: the path is empty";
        return false;
    }

    const std::string temp_path = final_path + std::string{kTempSuffix};
    std::string write_error;
    if (!write_bytes(temp_path, contents, write_error)) {
        std::string ignored;
        remove_file(temp_path, ignored);
        error = std::move(write_error);
        return false;
    }
    if (!replace_file(temp_path, final_path, error)) {
        std::string ignored;
        remove_file(temp_path, ignored);
        return false;
    }
    return true;
}

bool remove_stale_sidecar_temp(const std::string& output_path, std::string& error) {
    return remove_file(resume_sidecar_temp_path(output_path), error);
}

} // namespace flowget::detail

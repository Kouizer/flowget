#include "file_replace.h"

#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "download_util.h"
#include "utf8_path.h"

#include <cstddef>
#include <cstdint>
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
    // MoveFileExW(REPLACE_EXISTING) refuses to replace a destination that is open, even when the
    // reader passed FILE_SHARE_DELETE (observed as ERROR_ACCESS_DENIED). A sidecar reader - a UI
    // poller, a test helper - would therefore break the engine's own checkpoint. ReplaceFileW is
    // the Windows API for this case: it replaces an existing file while respecting delete sharing
    // and keeps the replacement atomic. When the destination does not exist yet (first checkpoint)
    // there is nothing to replace, so a plain move is enough.
    const DWORD destination_attributes = ::GetFileAttributesW(wide_final.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES) {
        if (::ReplaceFileW(wide_final.c_str(),
                           wide_temp.c_str(),
                           nullptr,
                           REPLACEFILE_WRITE_THROUGH,
                           nullptr,
                           nullptr) == 0) {
            error = "cannot replace \"" + final_path + "\": " + win32_error_text(::GetLastError());
            return false;
        }
    } else if (::MoveFileExW(wide_temp.c_str(), wide_final.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
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

bool read_file_utf8(const std::string& utf8_path,
                    std::size_t limit,
                    std::string& contents,
                    std::string& error) {
    contents.clear();
    const std::unique_ptr<IStorage> storage = make_platform_storage();
    const StorageResult opened = storage->open(utf8_path, OpenMode::read_only);
    if (!opened.ok()) {
        error = "cannot open \"" + utf8_path + "\": " +
                (opened.message.empty() ? storage_error_name(opened.error) : opened.message);
        return false;
    }

    FileOffset size = 0;
    const StorageResult sized = storage->size(size);
    if (!sized.ok()) {
        error = "cannot query the size of \"" + utf8_path +
                "\": " + (sized.message.empty() ? storage_error_name(sized.error) : sized.message);
        return false;
    }
    if (size < 0 || static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(limit)) {
        error = "\"" + utf8_path + "\" is larger than the " + std::to_string(limit) + "-byte limit";
        return false;
    }

    contents.assign(static_cast<std::size_t>(size), '\0');
    if (contents.empty()) {
        return true;
    }

    std::size_t read = 0;
    const std::span<char> text{contents.data(), contents.size()};
    const std::span<std::byte> bytes = std::as_writable_bytes(text);
    const StorageResult result = storage->read_at(0, bytes.data(), bytes.size(), read);
    if (!result.ok() || read != bytes.size()) {
        error = "cannot read \"" + utf8_path + "\": " +
                (result.message.empty() ? storage_error_name(result.error) : result.message);
        contents.clear();
        return false;
    }
    return true;
}

} // namespace flowget::detail

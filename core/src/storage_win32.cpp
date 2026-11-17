#include <flowget/storage.h>
#include <flowget/storage_detail.h>

#include "storage_internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

namespace flowget {
namespace {

// WriteFile/ReadFile take a DWORD length. 4 MiB keeps a single syscall well below the limit while
// still absorbing the largest curl chunk without splitting.
constexpr std::size_t kMaxChunk = std::size_t{4} * 1024 * 1024;

bool utf8_to_wide(std::string_view utf8, std::wstring& out) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int input_length = static_cast<int>(utf8.size());
    // MB_ERR_INVALID_CHARS: a path that is not valid UTF-8 is a caller bug, not a file name to
    // guess at. Failing here is better than opening a differently named file.
    const int wide_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, nullptr, 0);
    if (wide_length <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(wide_length));
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, out.data(), wide_length);
    return converted == wide_length;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input_length = static_cast<int>(wide.size());
    const int utf8_length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), input_length, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(utf8_length), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), input_length, out.data(), utf8_length, nullptr, nullptr);
    if (converted != utf8_length) {
        return {};
    }
    return out;
}

std::string win32_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        text = wide_to_utf8(std::wstring_view(buffer, length));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (text.empty()) {
        text = "unknown error";
    }
    return text;
}

std::string describe(DWORD code, std::string_view operation, std::string_view detail) {
    std::string message(operation);
    if (!detail.empty()) {
        message += " failed for \"";
        message += detail;
        message += '"';
    } else {
        message += " failed";
    }
    message += ": ";
    message += win32_error_text(code);
    return message;
}

// FAT32/exFAT and some network shares cannot reserve clusters up front. That is a downgrade, not a
// download failure: the file is still extended below.
bool is_unsupported_reservation_error(DWORD code) {
    switch (code) {
    case ERROR_INVALID_FUNCTION:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return true;
    default:
        return false;
    }
}

class Win32Storage final : public IStorage {
public:
    Win32Storage() = default;

    ~Win32Storage() override {
        close();
    }

    // The file handle has exactly one owner: copying would close it twice, moving would leave the
    // source pointing at a handle it no longer owns. Storage is created and held through IStorage
    // (see create_storage), so neither operation is ever needed.
    Win32Storage(const Win32Storage&) = delete;
    Win32Storage& operator=(const Win32Storage&) = delete;
    Win32Storage(Win32Storage&&) = delete;
    Win32Storage& operator=(Win32Storage&&) = delete;

    StorageResult open(const std::string& utf8_path, OpenMode mode) override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            return detail::make_storage_error(
                StorageError::already_open, 0, "storage is already open");
        }
        std::wstring wide_path;
        if (!utf8_to_wide(utf8_path, wide_path)) {
            return detail::invalid_argument_error("path is empty or not valid UTF-8");
        }

        const DWORD access =
            mode == OpenMode::read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        DWORD disposition = OPEN_EXISTING;
        if (mode == OpenMode::read_write) {
            disposition = OPEN_ALWAYS;
        } else if (mode == OpenMode::read_write_truncate) {
            disposition = CREATE_ALWAYS;
        }

        // Reads and writes always carry their own offset in an OVERLAPPED, so all workers can share
        // one handle. The file pointer is still updated by the system, but nobody reads it.
        const HANDLE handle = CreateFileW(
            wide_path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return detail::make_storage_error(StorageError::open,
                                              static_cast<int>(GetLastError()),
                                              describe(GetLastError(), "CreateFileW", utf8_path));
        }

        handle_ = handle;
        path_ = utf8_path;
        // reopen() must never truncate, so the truncate mode is downgraded after the open
        // succeeded.
        mode_ = mode == OpenMode::read_write_truncate ? OpenMode::read_write : mode;
        return {};
    }

    StorageResult reopen() override {
        if (path_.empty()) {
            return detail::invalid_argument_error("reopen() before a successful open()");
        }
        // Copy first: close() must not invalidate what we reopen with.
        const std::string path = path_;
        const OpenMode mode = mode_;
        const StorageResult closed = close();
        if (!closed.ok()) {
            return closed;
        }
        return open(path, mode);
    }

    StorageResult close() override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return {};
        }
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        if (CloseHandle(handle) == FALSE) {
            return detail::make_storage_error(StorageError::close,
                                              static_cast<int>(GetLastError()),
                                              describe(GetLastError(), "CloseHandle", path_));
        }
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    StorageResult preallocate(FileOffset size) override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return detail::not_open_error();
        }
        if (size < 0) {
            return detail::invalid_argument_error("preallocate() size is negative");
        }
        if (size == 0) {
            return {};
        }

        FileOffset current = 0;
        StorageResult queried = this->size(current);
        if (!queried.ok()) {
            return queried;
        }
        if (current >= size) {
            // Never shrink: a resume must not destroy bytes that are already on disk.
            return {};
        }

        // FileAllocationInfo reserves clusters. Documented contract: EOF must be <= allocation
        // size, but setting it does not necessarily extend EOF, so the size is set explicitly
        // below.
        FILE_ALLOCATION_INFO allocation{};
        allocation.AllocationSize.QuadPart = size;
        StorageResult reservation;
        if (SetFileInformationByHandle(
                handle_, FileAllocationInfo, &allocation, static_cast<DWORD>(sizeof(allocation))) ==
            FALSE) {
            const DWORD code = GetLastError();
            if (!is_unsupported_reservation_error(code)) {
                return detail::make_storage_error(StorageError::preallocate,
                                                  static_cast<int>(code),
                                                  describe(code,
                                                           "SetFileInformationByHandle("
                                                           "FileAllocationInfo)",
                                                           path_));
            }
            reservation = detail::make_storage_error(
                StorageError::preallocate_unsupported,
                static_cast<int>(code),
                describe(code, "SetFileInformationByHandle(FileAllocationInfo)", path_));
        }

        // FileEndOfFileInfo extends the file without moving the shared file pointer, so
        // preallocate() stays harmless for concurrent positional writers.
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = size;
        if (SetFileInformationByHandle(
                handle_, FileEndOfFileInfo, &eof, static_cast<DWORD>(sizeof(eof))) == FALSE) {
            const DWORD code = GetLastError();
            return detail::make_storage_error(StorageError::preallocate,
                                              static_cast<int>(code),
                                              describe(code,
                                                       "SetFileInformationByHandle("
                                                       "FileEndOfFileInfo)",
                                                       path_));
        }
        return reservation;
    }

    StorageResult write_at(FileOffset offset,
                           const std::byte* data,
                           std::size_t size,
                           std::size_t& written) override {
        written = 0;
        if (handle_ == INVALID_HANDLE_VALUE) {
            return detail::not_open_error();
        }
        if (data == nullptr && size > 0) {
            return detail::invalid_argument_error("write_at() buffer is null");
        }
        if (!detail::offset_range_valid(offset, size)) {
            return detail::invalid_argument_error("write_at() offset or size is out of range");
        }
        if (size == 0) {
            return {};
        }

        const detail::IoOutcome outcome =
            detail::io_all(offset,
                           size,
                           StorageError::write,
                           [this, data, offset](FileOffset current, std::size_t remaining) {
                               detail::IoAttempt attempt;
                               const std::size_t chunk = std::min(remaining, kMaxChunk);
                               const auto base = static_cast<std::uint64_t>(current);
                               // A caller-stack OVERLAPPED with an explicit offset is a positional
                               // write on a synchronous handle: WriteFile starts at that offset,
                               // ignores the shared file pointer for addressing, and returns only
                               // when the operation is complete. No cross-worker lock is needed.
                               OVERLAPPED overlapped{};
                               overlapped.Offset = static_cast<DWORD>(base & 0xFFFFFFFFULL);
                               overlapped.OffsetHigh = static_cast<DWORD>(base >> 32U);
                               DWORD bytes_written = 0;
                               const std::byte* chunk_data =
                                   data + static_cast<std::size_t>(current - offset);
                               if (WriteFile(handle_,
                                             chunk_data,
                                             static_cast<DWORD>(chunk),
                                             &bytes_written,
                                             &overlapped) == FALSE) {
                                   attempt.failed = true;
                                   attempt.platform_code = static_cast<int>(GetLastError());
                                   attempt.retryable = false;
                                   return attempt;
                               }
                               attempt.bytes = static_cast<std::size_t>(bytes_written);
                               return attempt;
                           });
        written = outcome.transferred;
        if (outcome.error == StorageError::none) {
            return {};
        }
        std::string message = "WriteFile failed at offset ";
        message += std::to_string(offset);
        message += " after ";
        message += std::to_string(outcome.transferred);
        message += " bytes";
        return detail::make_storage_error(outcome.error, outcome.platform_code, std::move(message));
    }

    StorageResult
    read_at(FileOffset offset, std::byte* data, std::size_t size, std::size_t& read) override {
        read = 0;
        if (handle_ == INVALID_HANDLE_VALUE) {
            return detail::not_open_error();
        }
        if (data == nullptr && size > 0) {
            return detail::invalid_argument_error("read_at() buffer is null");
        }
        if (!detail::offset_range_valid(offset, size)) {
            return detail::invalid_argument_error("read_at() offset or size is out of range");
        }
        if (size == 0) {
            return {};
        }

        const detail::IoOutcome outcome = detail::io_all(
            offset,
            size,
            StorageError::read,
            [this, data, offset](FileOffset current, std::size_t remaining) {
                detail::IoAttempt attempt;
                const std::size_t chunk = std::min(remaining, kMaxChunk);
                const auto base = static_cast<std::uint64_t>(current);
                OVERLAPPED overlapped{};
                overlapped.Offset = static_cast<DWORD>(base & 0xFFFFFFFFULL);
                overlapped.OffsetHigh = static_cast<DWORD>(base >> 32U);
                DWORD bytes_read = 0;
                std::byte* chunk_data = data + static_cast<std::size_t>(current - offset);
                if (ReadFile(
                        handle_, chunk_data, static_cast<DWORD>(chunk), &bytes_read, &overlapped) ==
                    FALSE) {
                    attempt.failed = true;
                    attempt.platform_code = static_cast<int>(GetLastError());
                    attempt.retryable = false;
                    return attempt;
                }
                attempt.bytes = static_cast<std::size_t>(bytes_read);
                return attempt;
            });
        read = outcome.transferred;
        if (outcome.error == StorageError::none) {
            return {};
        }
        std::string message = "ReadFile returned a short read at offset ";
        message += std::to_string(offset);
        message += " (expected ";
        message += std::to_string(size);
        message += " bytes, got ";
        message += std::to_string(outcome.transferred);
        message += ')';
        return detail::make_storage_error(outcome.error, outcome.platform_code, std::move(message));
    }

    StorageResult flush() override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return detail::not_open_error();
        }
        if (FlushFileBuffers(handle_) == FALSE) {
            const DWORD code = GetLastError();
            return detail::make_storage_error(StorageError::flush,
                                              static_cast<int>(code),
                                              describe(code, "FlushFileBuffers", path_));
        }
        return {};
    }

    StorageResult size(FileOffset& out) override {
        out = 0;
        if (handle_ == INVALID_HANDLE_VALUE) {
            return detail::not_open_error();
        }
        LARGE_INTEGER file_size{};
        if (GetFileSizeEx(handle_, &file_size) == FALSE) {
            const DWORD code = GetLastError();
            return detail::make_storage_error(StorageError::query_size,
                                              static_cast<int>(code),
                                              describe(code, "GetFileSizeEx", path_));
        }
        out = static_cast<FileOffset>(file_size.QuadPart);
        return {};
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    // Remembered so reopen() can use the same file. Kept after close() on purpose.
    std::string path_;
    OpenMode mode_ = OpenMode::read_write;
};

} // namespace

std::unique_ptr<IStorage> make_platform_storage() {
    return std::make_unique<Win32Storage>();
}

} // namespace flowget

#ifndef FLOWGET_STORAGE_H
#define FLOWGET_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace flowget {

// Positional file offsets are signed 64-bit on every platform. A negative offset is a caller bug, and
// off_t is only guaranteed to be 64-bit because core/CMakeLists.txt defines _FILE_OFFSET_BITS=64 on
// POSIX (32-bit Android would otherwise use a 32-bit off_t and silently truncate large downloads).
using FileOffset = std::int64_t;

// Error taxonomy of the storage layer. The segmented engine (S4/S5) maps these onto its own retry
// decisions; nothing in core throws across a boundary (ADR-0002).
enum class StorageError {
    none = 0,
    // Caller bug: negative offset, null buffer, path that is not valid UTF-8, offset+size overflow.
    invalid_argument,
    // I/O before open(), or after close().
    not_open,
    // open() called on an already open storage; close() first.
    already_open,
    // The file or directory could not be opened/created.
    open,
    read,
    write,
    // Space could not be reserved: ENOSPC, EIO, a quota, a bad sector. Fatal for a download.
    preallocate,
    // The filesystem cannot reserve space up front (FAT32, some network shares, ZFS/overlayfs).
    // The file was still extended to the requested size, so this is a warning, not a failure.
    preallocate_unsupported,
    flush,
    close,
    // Size/metadata query failed.
    query_size,
};

const char* storage_error_name(StorageError error) noexcept;

// False only for `none` and `preallocate_unsupported`: a caller that treats every non-none result as a
// failure would abort downloads for a harmless "cannot reserve clusters" downgrade.
bool storage_error_is_fatal(StorageError error) noexcept;

struct StorageResult {
    StorageError error = StorageError::none;
    // GetLastError() on Windows, errno (or the posix_fallocate return code) on POSIX. 0 when none.
    int platform_code = 0;
    std::string message;

    [[nodiscard]] bool ok() const {
        return error == StorageError::none;
    }
};

enum class OpenMode {
    // Existing file only; writes fail with EBADF/access denied.
    read_only = 0,
    // Create when missing, keep existing bytes. This is the resume path (S5): an interrupted file must
    // not be truncated before its sidecar has been validated.
    read_write,
    // Create when missing and truncate to zero. Fresh download.
    read_write_truncate,
};

// Positional file access for the segmented download engine.
//
// Thread safety contract:
//   * write_at() and read_at() may be called concurrently from any number of threads on the same
//     instance. Neither implementation keeps a shared file position (POSIX pwrite/pread; on Windows a
//     caller-stack OVERLAPPED carries the offset), so workers never serialize behind each other.
//   * open(), reopen(), close(), preallocate(), flush() and size() require exclusive access: the
//     engine calls them while no worker is writing (before start, after join, at a checkpoint).
//
// Paths are UTF-8, not std::filesystem::path. On Windows a path built from a narrow std::string is
// interpreted in the ANSI code page, which mangles Turkish/CJK/emoji names ("Varsayılan Proje" lesson);
// converting UTF-8 to UTF-16 inside the implementation makes the correct behaviour the default instead
// of a rule the next author has to remember. The C ABI also carries UTF-8.
class IStorage {
public:
    IStorage() = default;
    virtual ~IStorage() = default;
    IStorage(const IStorage&) = delete;
    IStorage& operator=(const IStorage&) = delete;
    IStorage(IStorage&&) = delete;
    IStorage& operator=(IStorage&&) = delete;

    // No-op returning ok() when already open. On failure the storage stays closed, but the path/mode
    // are remembered so reopen() can retry.
    virtual StorageResult open(const std::string& utf8_path,
                               OpenMode mode = OpenMode::read_write) = 0;

    // Closes and reopens with the path of the last successful open(). Never truncates: a storage that
    // was opened with read_write_truncate reopens as read_write, because truncation is a creation-time
    // decision and reopen() must not destroy bytes (S5 re-validates a file after a crash with this).
    virtual StorageResult reopen() = 0;

    // Idempotent: closing an already closed storage returns ok(). A failed close leaves the storage
    // closed (the handle is released either way).
    virtual StorageResult close() = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    // Reserves `size` bytes and extends the file to exactly `size`, matching posix_fallocate. Never
    // shrinks: a request smaller than the current size is a no-op. On filesystems that cannot reserve
    // space the file is still extended and `preallocate_unsupported` is returned.
    virtual StorageResult preallocate(FileOffset size) = 0;

    // Writes `size` bytes at `offset`, looping over partial writes. On failure `written` holds the
    // bytes that did reach the file, so a retry (S6) can continue at offset + written.
    virtual StorageResult write_at(FileOffset offset,
                                   const std::byte* data,
                                   std::size_t size,
                                   std::size_t& written) = 0;

    // Reads `size` bytes at `offset`. A short read means the file is shorter than the caller expected,
    // so it is reported as an error; `read` holds the bytes that were available.
    virtual StorageResult read_at(FileOffset offset,
                                  std::byte* data,
                                  std::size_t size,
                                  std::size_t& read) = 0;

    // fsync / FlushFileBuffers: makes completed writes durable (S5 checkpoint, pause, exit).
    virtual StorageResult flush() = 0;

    virtual StorageResult size(FileOffset& out) = 0;
};

// Creates the platform implementation (Win32Storage on Windows, PosixStorage elsewhere).
std::unique_ptr<IStorage> make_platform_storage();

} // namespace flowget

#endif /* FLOWGET_STORAGE_H */

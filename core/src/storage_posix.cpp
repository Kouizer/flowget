#include <flowget/storage.h>
#include <flowget/storage_detail.h>

#include "storage_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// off_t is 32-bit on a 32-bit Android build unless _FILE_OFFSET_BITS=64 is defined;
// core/CMakeLists.txt sets it on the target. These assertions turn a silently truncated >2 GiB
// download into a build failure.
static_assert(std::is_signed_v<off_t>, "off_t must be signed for the -1 error convention");
static_assert(sizeof(off_t) >= sizeof(std::int64_t), "off_t must hold a 64-bit file offset");

namespace flowget {
namespace {

// pwrite/pread take size_t, but a single call is kept small so a partial transfer is a normal event
// rather than a rare one, and so one worker cannot monopolize the file descriptor for long.
constexpr std::size_t kMaxChunk = std::size_t{4} * 1024 * 1024;

bool is_retryable_errno(int code) {
    return code == EINTR || code == EAGAIN || code == EWOULDBLOCK;
}

std::string describe(int code, std::string_view operation, std::string_view detail) {
    std::string message(operation);
    if (!detail.empty()) {
        message += " failed for \"";
        message += detail;
        message += '"';
    } else {
        message += " failed";
    }
    message += ": ";
    const char* text = std::strerror(code);
    message += text != nullptr ? text : "unknown error";
    return message;
}

class PosixStorage final : public IStorage {
public:
    PosixStorage() = default;

    ~PosixStorage() override {
        close();
    }

    // The file descriptor has exactly one owner: copying would close it twice, moving would leave
    // the source pointing at a descriptor it no longer owns. Storage is created and held through
    // IStorage (see create_storage), so neither operation is ever needed.
    PosixStorage(const PosixStorage&) = delete;
    PosixStorage& operator=(const PosixStorage&) = delete;
    PosixStorage(PosixStorage&&) = delete;
    PosixStorage& operator=(PosixStorage&&) = delete;

    StorageResult open(const std::string& utf8_path, OpenMode mode) override {
        if (fd_ >= 0) {
            return detail::make_storage_error(
                StorageError::already_open, 0, "storage is already open");
        }
        if (utf8_path.empty()) {
            return detail::invalid_argument_error("path is empty");
        }

        int flags = O_CLOEXEC;
        mode_t create_mode = 0;
        if (mode == OpenMode::read_only) {
            flags |= O_RDONLY;
        } else {
            flags |= O_RDWR | O_CREAT;
            create_mode = static_cast<mode_t>(0644);
            if (mode == OpenMode::read_write_truncate) {
                flags |= O_TRUNC;
            }
        }

        // O_CLOEXEC matters beyond hygiene: M5/M6 spawn yt-dlp and FFmpeg children, and a leaking
        // download fd keeps the file open in an unrelated process.
        const int fd = ::open(utf8_path.c_str(), flags, create_mode);
        if (fd < 0) {
            const int code = errno;
            return detail::make_storage_error(
                StorageError::open, code, describe(code, "open", utf8_path));
        }

        fd_ = fd;
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
        const std::string path = path_;
        const OpenMode mode = mode_;
        // Not const: returning a const local forces a copy instead of a move.
        StorageResult closed = close();
        if (!closed.ok()) {
            return closed;
        }
        return open(path, mode);
    }

    StorageResult close() override {
        if (fd_ < 0) {
            return {};
        }
        const int fd = fd_;
        fd_ = -1;
        // EINTR is not retried: POSIX leaves the state of the descriptor unspecified after a failed
        // close and Linux releases it anyway. Retrying could close a recycled descriptor.
        if (::close(fd) != 0) {
            const int code = errno;
            return detail::make_storage_error(
                StorageError::close, code, describe(code, "close", path_));
        }
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return fd_ >= 0;
    }

    StorageResult preallocate(FileOffset size) override {
        if (fd_ < 0) {
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

        // posix_fallocate returns the error code directly; it does not set errno.
        const int code = ::posix_fallocate(fd_, 0, static_cast<off_t>(size));
        if (code == 0) {
            return {};
        }

        if (code == EOPNOTSUPP || code == ENOSYS || code == EINVAL) {
            // The filesystem cannot reserve space (overlayfs, some network mounts). Extend the file
            // instead so the final size is right, and report the downgrade so it can be logged.
            int truncated = 0;
            do {
                truncated = ::ftruncate(fd_, static_cast<off_t>(size));
            } while (truncated != 0 && errno == EINTR);
            if (truncated != 0) {
                const int truncate_code = errno;
                return detail::make_storage_error(StorageError::preallocate,
                                                  truncate_code,
                                                  describe(truncate_code, "ftruncate", path_));
            }
            return detail::make_storage_error(StorageError::preallocate_unsupported,
                                              code,
                                              describe(code, "posix_fallocate", path_));
        }
        return detail::make_storage_error(
            StorageError::preallocate, code, describe(code, "posix_fallocate", path_));
    }

    StorageResult write_at(FileOffset offset,
                           const std::byte* data,
                           std::size_t size,
                           std::size_t& written) override {
        written = 0;
        if (fd_ < 0) {
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
                               const std::byte* chunk_data =
                                   data + static_cast<std::size_t>(current - offset);
                               // pwrite never touches the shared file position, so workers write
                               // independently on the same descriptor.
                               const ssize_t result =
                                   ::pwrite(fd_, chunk_data, chunk, static_cast<off_t>(current));
                               if (result < 0) {
                                   attempt.failed = true;
                                   attempt.platform_code = errno;
                                   attempt.retryable = is_retryable_errno(errno);
                                   return attempt;
                               }
                               // Only positive results reach the cast, so no sign narrowing.
                               attempt.bytes = static_cast<std::size_t>(result);
                               return attempt;
                           });
        written = outcome.transferred;
        if (outcome.error == StorageError::none) {
            return {};
        }
        std::string message = "pwrite failed at offset ";
        message += std::to_string(offset);
        message += " after ";
        message += std::to_string(outcome.transferred);
        message += " bytes";
        return detail::make_storage_error(outcome.error, outcome.platform_code, std::move(message));
    }

    StorageResult
    read_at(FileOffset offset, std::byte* data, std::size_t size, std::size_t& read) override {
        read = 0;
        if (fd_ < 0) {
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
                std::byte* chunk_data = data + static_cast<std::size_t>(current - offset);
                const ssize_t result = ::pread(fd_, chunk_data, chunk, static_cast<off_t>(current));
                if (result < 0) {
                    attempt.failed = true;
                    attempt.platform_code = errno;
                    attempt.retryable = is_retryable_errno(errno);
                    return attempt;
                }
                attempt.bytes = static_cast<std::size_t>(result);
                return attempt;
            });
        read = outcome.transferred;
        if (outcome.error == StorageError::none) {
            return {};
        }
        std::string message = "pread returned a short read at offset ";
        message += std::to_string(offset);
        message += " (expected ";
        message += std::to_string(size);
        message += " bytes, got ";
        message += std::to_string(outcome.transferred);
        message += ')';
        return detail::make_storage_error(outcome.error, outcome.platform_code, std::move(message));
    }

    StorageResult flush() override {
        if (fd_ < 0) {
            return detail::not_open_error();
        }
        int result = 0;
        do {
            result = ::fsync(fd_);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            const int code = errno;
            return detail::make_storage_error(
                StorageError::flush, code, describe(code, "fsync", path_));
        }
        return {};
    }

    StorageResult size(FileOffset& out) override {
        out = 0;
        if (fd_ < 0) {
            return detail::not_open_error();
        }
        struct stat info{};
        if (::fstat(fd_, &info) != 0) {
            const int code = errno;
            return detail::make_storage_error(
                StorageError::query_size, code, describe(code, "fstat", path_));
        }
        out = static_cast<FileOffset>(info.st_size);
        return {};
    }

private:
    int fd_ = -1;
    // Remembered so reopen() can use the same file. Kept after close() on purpose.
    std::string path_;
    OpenMode mode_ = OpenMode::read_write;
};

} // namespace

std::unique_ptr<IStorage> make_platform_storage() {
    return std::make_unique<PosixStorage>();
}

} // namespace flowget

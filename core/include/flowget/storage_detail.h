#ifndef FLOWGET_STORAGE_DETAIL_H
#define FLOWGET_STORAGE_DETAIL_H

#include <flowget/storage.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// Internal seam, not part of the C ABI. The partial-write/EINTR loop is shared by both platform
// implementations and is tested with fake syscalls: forcing a real filesystem to return a short
// write or EINTR is not possible deterministically, and an untested retry loop in a download engine
// is how silent truncation ships.

namespace flowget::detail {

// One raw syscall attempt. `attempt` never throws.
struct IoAttempt {
    bool failed = false;
    // EINTR/EAGAIN: the same offset can be retried immediately. Always false on Windows.
    bool retryable = false;
    std::size_t bytes = 0;
    // GetLastError() / errno of the failed attempt. 0 when the syscall failed without a code.
    int platform_code = 0;
};

struct IoOutcome {
    StorageError error = StorageError::none;
    std::size_t transferred = 0;
    int platform_code = 0;
};

// Attempts that move no bytes before the loop gives up. EINTR storms are rare; 100 covers
// signal-heavy environments while a filesystem that keeps refusing cannot spin forever.
inline constexpr int kMaxAttemptsWithoutProgress = 100;

// True when [offset, offset + size) is representable: offset is non-negative and the end does not
// overflow FileOffset. Checked before any cast to off_t or OVERLAPPED, so a wrapped offset can
// never be written to a small offset instead.
inline bool offset_range_valid(FileOffset offset, std::size_t size) noexcept {
    if (offset < 0) {
        return false;
    }
    const auto start = static_cast<std::uint64_t>(offset);
    const auto length = static_cast<std::uint64_t>(size);
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<FileOffset>::max());
    return length <= limit && start <= limit - length;
}

// Repeats `attempt(offset, size)` until `size` bytes are transferred or an error is not worth
// retrying. `failure` is the error reported for a failed attempt (StorageError::read or ::write).
//
// Semantics that matter for a download engine:
//   * a short transfer (returned bytes < requested) advances the offset and continues;
//   * EINTR/EAGAIN retries the same offset, up to kMaxAttemptsWithoutProgress times in a row;
//   * a zero-byte return is an error, never a retry: on a positional read it means end-of-file, on
//   a
//     write it means the filesystem made no progress.
// The callable is taken by value: io_all invokes it once per attempt, so a forwarding reference
// would either never be forwarded or be moved from repeatedly.
template <typename AttemptFn>
IoOutcome io_all(FileOffset offset, std::size_t size, StorageError failure, AttemptFn attempt) {
    IoOutcome outcome;
    int attempts_without_progress = 0;
    while (outcome.transferred < size) {
        const std::size_t remaining = size - outcome.transferred;
        const FileOffset current = offset + static_cast<FileOffset>(outcome.transferred);
        const IoAttempt result = attempt(current, remaining);

        if (result.failed || result.bytes == 0) {
            if (result.failed && result.retryable &&
                ++attempts_without_progress <= kMaxAttemptsWithoutProgress) {
                continue;
            }
            outcome.error = failure;
            outcome.platform_code = result.platform_code;
            return outcome;
        }
        if (result.bytes > remaining) {
            // A syscall cannot report more than it was asked for. Treat it as a broken driver
            // rather than advancing the offset past the buffer.
            outcome.error = StorageError::invalid_argument;
            outcome.platform_code = result.platform_code;
            return outcome;
        }
        outcome.transferred += result.bytes;
        attempts_without_progress = 0;
    }
    return outcome;
}

} // namespace flowget::detail

#endif /* FLOWGET_STORAGE_DETAIL_H */

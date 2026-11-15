#include <flowget/storage.h>
#include <flowget/storage_detail.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::detail::IoAttempt;
using flowget::detail::IoOutcome;
using flowget::detail::io_all;
using flowget::FileOffset;
using flowget::OpenMode;
using flowget::StorageError;
using flowget::StorageResult;

constexpr StorageError kWrite = StorageError::write;
constexpr StorageError kRead = StorageError::read;

TEST(StorageErrorTest, NamesAndFatality) {
    EXPECT_STREQ(flowget::storage_error_name(StorageError::none), "none");
    EXPECT_STREQ(flowget::storage_error_name(StorageError::preallocate_unsupported),
                 "preallocate_unsupported");
    EXPECT_STREQ(flowget::storage_error_name(StorageError::query_size), "query_size");

    EXPECT_FALSE(flowget::storage_error_is_fatal(StorageError::none));
    // A filesystem that cannot reserve clusters is a logged downgrade, not an aborted download.
    EXPECT_FALSE(flowget::storage_error_is_fatal(StorageError::preallocate_unsupported));
    for (const StorageError fatal :
         {StorageError::invalid_argument, StorageError::not_open, StorageError::already_open,
          StorageError::open, StorageError::read, StorageError::write, StorageError::preallocate,
          StorageError::flush, StorageError::close, StorageError::query_size}) {
        EXPECT_TRUE(flowget::storage_error_is_fatal(fatal))
            << flowget::storage_error_name(fatal);
    }
}

TEST(StorageErrorTest, OffsetRangeValidationRejectsOverflowAndNegatives) {
    EXPECT_TRUE(flowget::detail::offset_range_valid(0, 0));
    EXPECT_TRUE(flowget::detail::offset_range_valid(0, 4096));
    EXPECT_TRUE(flowget::detail::offset_range_valid(
        std::numeric_limits<FileOffset>::max() - 1, 1));
    // offset + size wraps past INT64_MAX: must not be cast to off_t.
    EXPECT_FALSE(flowget::detail::offset_range_valid(std::numeric_limits<FileOffset>::max(), 1));
    EXPECT_FALSE(flowget::detail::offset_range_valid(-1, 1));
}

TEST(IoAllTest, WritesEverythingInOneAttempt) {
    std::vector<std::byte> buffer(4096, std::byte{0x5A});
    const IoOutcome outcome = io_all(0, buffer.size(), kWrite,
                                     [&buffer](FileOffset, std::size_t remaining) {
                                         IoAttempt attempt;
                                         attempt.bytes = remaining;
                                         return attempt;
                                     });
    EXPECT_EQ(outcome.error, StorageError::none);
    EXPECT_EQ(outcome.transferred, buffer.size());
    EXPECT_EQ(outcome.platform_code, 0);
}

TEST(IoAllTest, LoopsOverPartialWrites) {
    std::vector<std::byte> buffer(64, std::byte{0});
    std::vector<FileOffset> offsets;
    const IoOutcome outcome =
        io_all(10, buffer.size(), kWrite,
               [&offsets](FileOffset offset, std::size_t) {
                   offsets.push_back(offset);
                   IoAttempt attempt;
                   attempt.bytes = 1; // the classic hostile filesystem: one byte per call
                   return attempt;
               });
    EXPECT_EQ(outcome.error, StorageError::none);
    EXPECT_EQ(outcome.transferred, buffer.size());
    ASSERT_EQ(offsets.size(), buffer.size());
    EXPECT_EQ(offsets.front(), 10);
    // Every call starts where the previous one stopped, never at the original offset.
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        EXPECT_EQ(offsets[index], offsets[index - 1] + 1);
    }
}

TEST(IoAllTest, RetriesInterruptedCallsAtTheSameOffset) {
    int calls = 0;
    const IoOutcome outcome =
        io_all(0, 8, kWrite,
               [&calls](FileOffset offset, std::size_t remaining) {
                   ++calls;
                   IoAttempt attempt;
                   if (calls <= 2) {
                       attempt.failed = true;
                       attempt.retryable = true;
                       attempt.platform_code = 4; // EINTR on Linux
                       return attempt;
                   }
                   EXPECT_EQ(offset, 0);
                   attempt.bytes = remaining;
                   return attempt;
               });
    EXPECT_EQ(outcome.error, StorageError::none);
    EXPECT_EQ(outcome.transferred, 8);
    EXPECT_EQ(calls, 3);
}

TEST(IoAllTest, GivesUpAfterTooManyInterruptions) {
    int calls = 0;
    const IoOutcome outcome = io_all(0, 8, kWrite,
                                     [&calls](FileOffset, std::size_t) {
                                         ++calls;
                                         IoAttempt attempt;
                                         attempt.failed = true;
                                         attempt.retryable = true;
                                         attempt.platform_code = 4;
                                         return attempt;
                                     });
    EXPECT_EQ(outcome.error, kWrite);
    EXPECT_EQ(outcome.transferred, 0);
    EXPECT_EQ(outcome.platform_code, 4);
    // A signal storm cannot spin forever: the loop stops right after the retry budget.
    EXPECT_EQ(calls, flowget::detail::kMaxAttemptsWithoutProgress + 1);

    // Exactly at the budget the call still succeeds, so the boundary is not off by one.
    calls = 0;
    const IoOutcome tolerated =
        io_all(0, 8, kWrite, [&calls](FileOffset, std::size_t remaining) {
            ++calls;
            IoAttempt attempt;
            if (calls <= flowget::detail::kMaxAttemptsWithoutProgress) {
                attempt.failed = true;
                attempt.retryable = true;
                attempt.platform_code = 4;
                return attempt;
            }
            attempt.bytes = remaining;
            return attempt;
        });
    EXPECT_EQ(tolerated.error, StorageError::none);
    EXPECT_EQ(tolerated.transferred, 8);
}

TEST(IoAllTest, ZeroByteReturnIsAnErrorNotARetry) {
    int calls = 0;
    const IoOutcome outcome = io_all(0, 16, kRead, [&calls](FileOffset, std::size_t) {
        ++calls;
        IoAttempt attempt;
        attempt.bytes = 0; // end-of-file on a positional read
        return attempt;
    });
    EXPECT_EQ(outcome.error, kRead);
    EXPECT_EQ(outcome.transferred, 0);
    EXPECT_EQ(calls, 1);
}

TEST(IoAllTest, ReportsBytesTransferredBeforeAFailure) {
    int calls = 0;
    const IoOutcome outcome = io_all(0, 16, kWrite, [&calls](FileOffset, std::size_t) {
        ++calls;
        IoAttempt attempt;
        if (calls == 1) {
            attempt.bytes = 6;
            return attempt;
        }
        attempt.failed = true;
        attempt.retryable = false;
        attempt.platform_code = 28; // ENOSPC
        return attempt;
    });
    EXPECT_EQ(outcome.error, kWrite);
    EXPECT_EQ(outcome.transferred, 6);
    EXPECT_EQ(outcome.platform_code, 28);
}

TEST(IoAllTest, RejectsAnAttemptThatReportsMoreBytesThanRequested) {
    const IoOutcome outcome = io_all(0, 4, kWrite, [](FileOffset, std::size_t) {
        IoAttempt attempt;
        attempt.bytes = 5;
        return attempt;
    });
    EXPECT_EQ(outcome.error, StorageError::invalid_argument);
    EXPECT_EQ(outcome.transferred, 0);
}

TEST(IoAllTest, EmptyBufferDoesNotCallTheSyscall) {
    int calls = 0;
    const IoOutcome outcome = io_all(0, 0, kWrite, [&calls](FileOffset, std::size_t) {
        ++calls;
        return IoAttempt{};
    });
    EXPECT_EQ(outcome.error, StorageError::none);
    EXPECT_EQ(outcome.transferred, 0);
    EXPECT_EQ(calls, 0);
}

// Argument validation that needs no filesystem access, so it runs on every platform.
TEST(StorageContractTest, RejectsCallsBeforeOpen) {
    const auto storage = flowget::make_platform_storage();
    ASSERT_NE(storage, nullptr);
    EXPECT_FALSE(storage->is_open());

    std::byte buffer[8] = {};
    std::size_t count = 99;
    EXPECT_EQ(storage->write_at(0, buffer, sizeof(buffer), count).error, StorageError::not_open);
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(storage->read_at(0, buffer, sizeof(buffer), count).error, StorageError::not_open);
    EXPECT_EQ(count, 0U);

    FileOffset size = 99;
    EXPECT_EQ(storage->size(size).error, StorageError::not_open);
    EXPECT_EQ(size, 0);
    EXPECT_EQ(storage->flush().error, StorageError::not_open);
    EXPECT_EQ(storage->preallocate(1024).error, StorageError::not_open);

    // close() is idempotent, including on a storage that was never opened.
    EXPECT_TRUE(storage->close().ok());

    const StorageResult reopened = storage->reopen();
    EXPECT_EQ(reopened.error, StorageError::invalid_argument);
    EXPECT_FALSE(reopened.message.empty());
}

TEST(StorageContractTest, ClosedStorageReportsNotOpenBeforeArgumentValidation) {
    const auto storage = flowget::make_platform_storage();
    std::byte buffer[4] = {};
    std::size_t count = 0;

    // not_open comes first: a caller that never opened the storage must not be told its arguments are
    // the problem. Argument validation on an open storage is covered by the integration tests.
    EXPECT_EQ(storage->write_at(-1, buffer, sizeof(buffer), count).error,
              StorageError::not_open);
    EXPECT_EQ(storage->read_at(-1, buffer, sizeof(buffer), count).error,
              StorageError::not_open);
    EXPECT_EQ(storage->write_at(0, nullptr, 4, count).error, StorageError::not_open);
    EXPECT_EQ(storage->preallocate(-1).error, StorageError::not_open);
}

TEST(StorageContractTest, EmptyBufferOnClosedStorageIsStillNotOpen) {
    const auto storage = flowget::make_platform_storage();
    std::size_t count = 7;
    EXPECT_EQ(storage->write_at(0, nullptr, 0, count).error, StorageError::not_open);
    EXPECT_EQ(count, 0U);
}

} // namespace

#include <flowget/storage.h>

#include <algorithm>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::FileOffset;
using flowget::IStorage;
using flowget::OpenMode;
using flowget::StorageError;
using flowget::StorageResult;

// UTF-8 bytes -> native path without passing through the ANSI code page. Building
// std::filesystem::path from std::string decodes the bytes in the ANSI code page on Windows and
// mangles every non-ASCII name (tasks/lessons.md, 2026-11-14). The test needs this only to create
// directories and to assert that the OS sees the same name; IStorage itself takes UTF-8 and
// converts internally.
std::filesystem::path utf8_path(const std::string& utf8) {
    const auto* begin = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string(begin, utf8.size()));
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::byte> make_pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::byte> data(size);
    std::uint32_t state = 0x9e3779b9U ^ static_cast<std::uint32_t>(seed);
    for (std::byte& byte : data) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<std::byte>(state & 0xFFU);
    }
    return data;
}

// A filesystem that cannot reserve clusters still extends the file, which is a documented
// downgrade.
void expect_preallocate_usable(const StorageResult& result) {
    EXPECT_TRUE(result.ok() || result.error == StorageError::preallocate_unsupported)
        << flowget::storage_error_name(result.error) << ": " << result.message;
}

// Compares `count` bytes starting at `actual_begin`/`expected_begin`. The slice checks read a small
// buffer against a window of the full expectation, so the two indices move independently.
void expect_bytes_equal(const std::vector<std::byte>& actual,
                        std::size_t actual_begin,
                        const std::vector<std::byte>& expected,
                        std::size_t expected_begin,
                        std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        ASSERT_EQ(static_cast<unsigned>(actual[actual_begin + index]),
                  static_cast<unsigned>(expected[expected_begin + index]))
            << "first mismatch at offset " << expected_begin + index;
    }
}

class StorageIoTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code error;
        std::filesystem::remove_all(root(), error);
        std::filesystem::create_directories(root(), error);
        ASSERT_FALSE(error) << error.message();
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root(), ignored);
    }

    static std::filesystem::path root() {
        return std::filesystem::current_path() / "storage_test_artifacts";
    }

    // Path handed to IStorage: UTF-8, exactly like the production code paths.
    static std::string storage_path(const std::string& name_utf8) {
        return to_utf8(root() / utf8_path(name_utf8));
    }
};

TEST_F(StorageIoTest, WritesAtOffsetAndReadsBack) {
    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("offsets.bin"), OpenMode::read_write_truncate).ok());
    ASSERT_TRUE(storage->is_open());

    const StorageResult preallocated = storage->preallocate(64 * 1024);
    expect_preallocate_usable(preallocated);

    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 64 * 1024);

    const std::vector<std::byte> pattern = make_pattern(1024, 7);
    std::size_t written = 0;
    const StorageResult write_result =
        storage->write_at(4096, pattern.data(), pattern.size(), written);
    ASSERT_TRUE(write_result.ok()) << write_result.message;
    EXPECT_EQ(written, pattern.size());

    // The preallocated region before the write must read back as zeros, not as garbage.
    std::vector<std::byte> zeros(4096);
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(0, zeros.data(), zeros.size(), read).ok());
    EXPECT_EQ(read, zeros.size());
    EXPECT_TRUE(std::all_of(
        zeros.begin(), zeros.end(), [](std::byte byte) { return byte == std::byte{0}; }));

    std::vector<std::byte> read_back(pattern.size());
    ASSERT_TRUE(storage->read_at(4096, read_back.data(), read_back.size(), read).ok());
    EXPECT_EQ(read, read_back.size());
    expect_bytes_equal(read_back, 0, pattern, 0, pattern.size());

    EXPECT_TRUE(storage->close().ok());
    EXPECT_FALSE(storage->is_open());
    // Bytes are on disk, not only in a buffer that close() dropped.
    EXPECT_TRUE(std::filesystem::exists(root() / utf8_path("offsets.bin")));
    EXPECT_EQ(std::filesystem::file_size(root() / utf8_path("offsets.bin")),
              static_cast<std::uintmax_t>(64 * 1024));
}

TEST_F(StorageIoTest, PreallocateNeverShrinksExistingData) {
    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("grow.bin"), OpenMode::read_write_truncate).ok());

    const std::vector<std::byte> head = make_pattern(4096, 3);
    std::size_t written = 0;
    ASSERT_TRUE(storage->write_at(0, head.data(), head.size(), written).ok());

    expect_preallocate_usable(storage->preallocate(1024 * 1024));
    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 1024 * 1024);

    // A smaller request is a no-op: a resume must never destroy already downloaded bytes.
    expect_preallocate_usable(storage->preallocate(2048));
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 1024 * 1024);

    std::vector<std::byte> read_back(head.size());
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(0, read_back.data(), read_back.size(), read).ok());
    expect_bytes_equal(read_back, 0, head, 0, head.size());
}

// The S4 contract: eight workers write disjoint ranges of one file at the same time through one
// IStorage instance. A shared file position or a missing OVERLAPPED offset would scramble the file.
TEST_F(StorageIoTest, EightThreadsWriteDisjointRangesConcurrently) {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kChunk = 256U * 1024U;
    constexpr std::size_t kTotal = kThreads * kChunk;

    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("threads.bin"), OpenMode::read_write_truncate).ok());
    expect_preallocate_usable(storage->preallocate(static_cast<FileOffset>(kTotal)));

    std::vector<std::byte> expected(kTotal);
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
        const std::vector<std::byte> pattern =
            make_pattern(kChunk, static_cast<std::uint8_t>(thread));
        std::copy(pattern.begin(),
                  pattern.end(),
                  expected.begin() + static_cast<std::ptrdiff_t>(thread * kChunk));
    }

    std::vector<StorageResult> results(kThreads);
    std::vector<std::size_t> written(kThreads, 0);
    // Every worker waits at the barrier so the writes really overlap.
    std::barrier start(static_cast<std::ptrdiff_t>(kThreads));
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&storage, &results, &written, &expected, &start, thread] {
            start.arrive_and_wait();
            const std::size_t offset = thread * kChunk;
            results[thread] = storage->write_at(
                static_cast<FileOffset>(offset), expected.data() + offset, kChunk, written[thread]);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    for (std::size_t thread = 0; thread < kThreads; ++thread) {
        EXPECT_TRUE(results[thread].ok())
            << "worker " << thread << ": " << flowget::storage_error_name(results[thread].error)
            << ": " << results[thread].message;
        EXPECT_EQ(written[thread], kChunk) << "worker " << thread;
    }

    std::vector<std::byte> actual(kTotal);
    std::size_t read = 0;
    const StorageResult read_result = storage->read_at(0, actual.data(), actual.size(), read);
    ASSERT_TRUE(read_result.ok()) << read_result.message;
    EXPECT_EQ(read, kTotal);
    expect_bytes_equal(actual, 0, expected, 0, kTotal);

    // Also verify each worker's range on its own, so a whole-file match cannot hide a shifted
    // slice.
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
        std::vector<std::byte> slice(kChunk);
        const std::size_t offset = thread * kChunk;
        ASSERT_TRUE(
            storage->read_at(static_cast<FileOffset>(offset), slice.data(), slice.size(), read)
                .ok());
        EXPECT_EQ(read, kChunk);
        expect_bytes_equal(slice, 0, expected, offset, kChunk);
    }
}

TEST_F(StorageIoTest, ReopenKeepsContentsAndMode) {
    const auto storage = flowget::make_platform_storage();
    const std::string path = storage_path("reopen.bin");
    ASSERT_TRUE(storage->open(path, OpenMode::read_write).ok());

    const std::vector<std::byte> pattern = make_pattern(8192, 11);
    std::size_t written = 0;
    ASSERT_TRUE(storage->write_at(1024, pattern.data(), pattern.size(), written).ok());
    ASSERT_TRUE(storage->flush().ok());
    ASSERT_TRUE(storage->close().ok());

    ASSERT_TRUE(storage->reopen().ok());
    EXPECT_TRUE(storage->is_open());

    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 1024 + static_cast<FileOffset>(pattern.size()));

    std::vector<std::byte> read_back(pattern.size());
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(1024, read_back.data(), read_back.size(), read).ok());
    expect_bytes_equal(read_back, 0, pattern, 0, pattern.size());

    // reopen() remembered read_write, so the file is still writable.
    ASSERT_TRUE(storage->write_at(0, pattern.data(), 16, written).ok());
    EXPECT_EQ(written, 16U);
}

TEST_F(StorageIoTest, ReopenAfterTruncateModeKeepsData) {
    const auto storage = flowget::make_platform_storage();
    const std::string path = storage_path("reopen-truncate.bin");
    ASSERT_TRUE(storage->open(path, OpenMode::read_write_truncate).ok());

    const std::vector<std::byte> pattern = make_pattern(2048, 13);
    std::size_t written = 0;
    ASSERT_TRUE(storage->write_at(0, pattern.data(), pattern.size(), written).ok());
    ASSERT_TRUE(storage->close().ok());

    // Truncation is a creation-time decision: reopening must not delete a finished or resumed file.
    ASSERT_TRUE(storage->reopen().ok());
    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, static_cast<FileOffset>(pattern.size()));

    std::vector<std::byte> read_back(pattern.size());
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(0, read_back.data(), read_back.size(), read).ok());
    expect_bytes_equal(read_back, 0, pattern, 0, pattern.size());
}

TEST_F(StorageIoTest, TruncateModeEmptiesAnExistingFile) {
    const auto storage = flowget::make_platform_storage();
    const std::string path = storage_path("truncate.bin");
    ASSERT_TRUE(storage->open(path, OpenMode::read_write_truncate).ok());
    expect_preallocate_usable(storage->preallocate(5 * 1024 * 1024));
    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 5 * 1024 * 1024);
    ASSERT_TRUE(storage->close().ok());

    ASSERT_TRUE(storage->open(path, OpenMode::read_write_truncate).ok());
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 0);
}

TEST_F(StorageIoTest, ReadOnlyModeRejectsWritesButAllowsReads) {
    {
        const auto writer = flowget::make_platform_storage();
        ASSERT_TRUE(writer->open(storage_path("readonly.bin"), OpenMode::read_write_truncate).ok());
        const std::vector<std::byte> pattern = make_pattern(512, 5);
        std::size_t written = 0;
        ASSERT_TRUE(writer->write_at(0, pattern.data(), pattern.size(), written).ok());
    }

    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("readonly.bin"), OpenMode::read_only).ok());

    std::byte byte = std::byte{0};
    std::size_t written = 0;
    const StorageResult write_result = storage->write_at(0, &byte, 1, written);
    EXPECT_FALSE(write_result.ok());
    EXPECT_EQ(write_result.error, StorageError::write);
    EXPECT_EQ(written, 0U);

    std::byte buffer[8] = {};
    std::size_t read = 0;
    EXPECT_TRUE(storage->read_at(0, buffer, sizeof(buffer), read).ok());
    EXPECT_EQ(read, sizeof(buffer));
}

TEST_F(StorageIoTest, OpeningAMissingFileReadOnlyFails) {
    const auto storage = flowget::make_platform_storage();
    const StorageResult result =
        storage->open(storage_path("does-not-exist.bin"), OpenMode::read_only);
    EXPECT_EQ(result.error, StorageError::open);
    EXPECT_NE(result.platform_code, 0);
    EXPECT_FALSE(result.message.empty());
    EXPECT_FALSE(storage->is_open());
}

TEST_F(StorageIoTest, SecondOpenIsRejectedUntilClose) {
    const auto storage = flowget::make_platform_storage();
    const std::string path = storage_path("double-open.bin");
    ASSERT_TRUE(storage->open(path, OpenMode::read_write_truncate).ok());
    EXPECT_EQ(storage->open(path, OpenMode::read_write).error, StorageError::already_open);
    ASSERT_TRUE(storage->close().ok());
    EXPECT_TRUE(storage->open(path, OpenMode::read_write).ok());
}

TEST_F(StorageIoTest, ReadingPastEndOfFileIsAnError) {
    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("short.bin"), OpenMode::read_write_truncate).ok());
    // A 0-byte file keeps the test independent of the filesystem's preallocation support.
    FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    ASSERT_EQ(size, 0);

    std::byte buffer[16] = {};
    std::size_t read = 99;
    const StorageResult result = storage->read_at(0, buffer, sizeof(buffer), read);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, StorageError::read);
    EXPECT_EQ(read, 0U);
    EXPECT_FALSE(result.message.empty());
}

TEST_F(StorageIoTest, InvalidArgumentsAreRejectedWhileOpen) {
    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path("args.bin"), OpenMode::read_write_truncate).ok());

    std::byte buffer[4] = {};
    std::size_t count = 0;
    EXPECT_EQ(storage->write_at(-1, buffer, sizeof(buffer), count).error,
              StorageError::invalid_argument);
    EXPECT_EQ(storage->read_at(-1, buffer, sizeof(buffer), count).error,
              StorageError::invalid_argument);
    EXPECT_EQ(storage->write_at(0, nullptr, sizeof(buffer), count).error,
              StorageError::invalid_argument);
    EXPECT_EQ(storage->read_at(0, nullptr, sizeof(buffer), count).error,
              StorageError::invalid_argument);
    EXPECT_EQ(storage->preallocate(-1).error, StorageError::invalid_argument);
    // offset + size overflows FileOffset: must be rejected, never wrapped to a small offset.
    EXPECT_EQ(
        storage->write_at(std::numeric_limits<FileOffset>::max(), buffer, sizeof(buffer), count)
            .error,
        StorageError::invalid_argument);

    FileOffset size = 0;
    EXPECT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, 0);
}

TEST_F(StorageIoTest, CloseIsIdempotent) {
    const auto storage = flowget::make_platform_storage();
    EXPECT_TRUE(storage->close().ok()); // never opened
    ASSERT_TRUE(storage->open(storage_path("close.bin"), OpenMode::read_write_truncate).ok());
    EXPECT_TRUE(storage->close().ok());
    EXPECT_TRUE(storage->close().ok());

    FileOffset size = 0;
    EXPECT_EQ(storage->size(size).error, StorageError::not_open);
}

// Turkish, CJK and emoji in both the directory and the file name: the UTF-8 -> UTF-16 conversion
// must not go through the ANSI code page. The old ofstream/path.string() path broke here on
// Windows.
TEST_F(StorageIoTest, NonAsciiPathRoundTrip) {
    const std::string directory_utf8 = "İndirilenler";
    const std::string file_utf8 = "İndirilen_çğış.bin";
    const std::filesystem::path directory = root() / utf8_path(directory_utf8);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error) << error.message();

    const std::string path = to_utf8(directory / utf8_path(file_utf8));
    const auto storage = flowget::make_platform_storage();
    const StorageResult opened = storage->open(path, OpenMode::read_write_truncate);
    ASSERT_TRUE(opened.ok()) << opened.message;

    const std::vector<std::byte> pattern = make_pattern(4096, 42);
    std::size_t written = 0;
    ASSERT_TRUE(storage->write_at(0, pattern.data(), pattern.size(), written).ok());
    ASSERT_TRUE(storage->flush().ok());
    ASSERT_TRUE(storage->close().ok());

    ASSERT_TRUE(storage->reopen().ok());
    std::vector<std::byte> read_back(pattern.size());
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(0, read_back.data(), read_back.size(), read).ok());
    expect_bytes_equal(read_back, 0, pattern, 0, pattern.size());

    // The OS must see the exact same name, not a replacement-character version of it.
    EXPECT_TRUE(std::filesystem::exists(directory / utf8_path(file_utf8)));
    EXPECT_EQ(std::filesystem::file_size(directory / utf8_path(file_utf8)),
              static_cast<std::uintmax_t>(pattern.size()));
}

TEST_F(StorageIoTest, NonAsciiFileNameWithEmojiRoundTrip) {
    const std::string file_utf8 = "şğü-中文-🚀.bin";
    const auto storage = flowget::make_platform_storage();
    ASSERT_TRUE(storage->open(storage_path(file_utf8), OpenMode::read_write_truncate).ok());

    const std::vector<std::byte> pattern = make_pattern(1024, 9);
    std::size_t written = 0;
    ASSERT_TRUE(storage->write_at(0, pattern.data(), pattern.size(), written).ok());

    std::vector<std::byte> read_back(pattern.size());
    std::size_t read = 0;
    ASSERT_TRUE(storage->read_at(0, read_back.data(), read_back.size(), read).ok());
    expect_bytes_equal(read_back, 0, pattern, 0, pattern.size());
    EXPECT_TRUE(std::filesystem::exists(root() / utf8_path(file_utf8)));
}

} // namespace

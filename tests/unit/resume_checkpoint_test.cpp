#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "resume_checkpoint.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace {

using flowget::FileOffset;
using flowget::IStorage;
using flowget::OpenMode;
using flowget::ResumeParseResult;
using flowget::ResumeSegment;
using flowget::ResumeState;
using flowget::StorageError;
using flowget::StorageResult;
using flowget::detail::CheckpointResult;
using flowget::detail::publish_resume_checkpoint;

// Minimal in-memory storage: the unit test only needs to control flush() so the "data is durable
// before the sidecar is published" invariant can be exercised without a real disk.
class FakeStorage final : public IStorage {
public:
    StorageResult open(const std::string& /*utf8_path*/, OpenMode /*mode*/) override {
        open_ = true;
        return {};
    }

    StorageResult reopen() override {
        return {};
    }

    StorageResult close() override {
        open_ = false;
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return open_;
    }

    StorageResult preallocate(FileOffset /*size*/) override {
        return {};
    }

    StorageResult write_at(FileOffset /*offset*/,
                           const std::byte* /*data*/,
                           std::size_t /*size*/,
                           std::size_t& written) override {
        written = 0;
        return {};
    }

    StorageResult read_at(FileOffset /*offset*/,
                          std::byte* /*data*/,
                          std::size_t /*size*/,
                          std::size_t& read) override {
        read = 0;
        return {};
    }

    StorageResult flush() override {
        ++flushes;
        return flush_result;
    }

    StorageResult size(FileOffset& out) override {
        out = 0;
        return {};
    }

    StorageResult flush_result;
    int flushes = 0;

private:
    bool open_ = false;
};

std::filesystem::path make_temp_dir() {
    const std::filesystem::path directory =
        std::filesystem::current_path() / "resume_checkpoint_artifacts";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

ResumeState sample_state() {
    ResumeState state;
    state.url = "https://example.com/big.bin";
    state.total_bytes = 4 * 1024 * 1024;
    state.etag = "\"abc123\"";
    state.last_modified = "Tue, 17 Nov 2026 10:00:00 GMT";
    state.segments = {{0, 2 * 1024 * 1024, 1024 * 1024}, {2 * 1024 * 1024, 4 * 1024 * 1024, 0}};
    return state;
}

TEST(ResumeCheckpoint, FlushesTheDataBeforePublishingTheSidecar) {
    const std::filesystem::path sidecar = make_temp_dir() / "out.bin.flowget";
    FakeStorage storage;
    const ResumeState state = sample_state();

    std::string error;
    EXPECT_EQ(publish_resume_checkpoint(storage, sidecar.string(), state, error),
              CheckpointResult::published)
        << error;
    EXPECT_EQ(storage.flushes, 1);
    EXPECT_EQ(read_text(sidecar), flowget::serialize_resume_state(state));
    EXPECT_FALSE(std::filesystem::exists(sidecar.string() + ".tmp"));

    const ResumeParseResult parsed = flowget::parse_resume_state(read_text(sidecar));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.state.bytes_done(), 1024 * 1024);
}

TEST(ResumeCheckpoint, FlushFailureLeavesThePreviousSidecarUntouched) {
    const std::filesystem::path sidecar = make_temp_dir() / "out.bin.flowget";
    write_text(sidecar, "previous sidecar\n");

    FakeStorage storage;
    storage.flush_result = StorageResult{StorageError::flush, 5, "disk went away"};
    const ResumeState state = sample_state();

    std::string error;
    EXPECT_EQ(publish_resume_checkpoint(storage, sidecar.string(), state, error),
              CheckpointResult::data_flush_failed);
    EXPECT_NE(error.find("flush"), std::string::npos);
    EXPECT_EQ(read_text(sidecar), "previous sidecar\n");
    EXPECT_FALSE(std::filesystem::exists(sidecar.string() + ".tmp"));
}

TEST(ResumeCheckpoint, RejectsAnEmptySidecarPath) {
    FakeStorage storage;
    std::string error;
    EXPECT_EQ(publish_resume_checkpoint(storage, "", sample_state(), error),
              CheckpointResult::sidecar_failed);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(storage.flushes, 0) << "no data should be flushed when there is nowhere to publish";
}

} // namespace

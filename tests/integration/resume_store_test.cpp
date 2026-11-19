#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "file_replace.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

using flowget::IStorage;
using flowget::OpenMode;
using flowget::resume_sidecar_path;
using flowget::resume_sidecar_temp_path;
using flowget::detail::read_file_utf8;

// UTF-8 -> native path without the ANSI code page (tasks/lessons.md, 2026-11-14). The sidecar
// helper itself takes UTF-8; the test needs this only to create directories and to read files back.
std::filesystem::path utf8_path(const std::string& utf8) {
    const auto* begin = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string(begin, utf8.size()));
}

std::string read_file(const std::string& utf8) {
    const std::unique_ptr<IStorage> storage = flowget::make_platform_storage();
    const flowget::StorageResult opened = storage->open(utf8, OpenMode::read_only);
    EXPECT_TRUE(opened.ok()) << opened.message;
    if (!opened.ok()) {
        return {};
    }
    flowget::FileOffset size = 0;
    const flowget::StorageResult sized = storage->size(size);
    EXPECT_TRUE(sized.ok()) << sized.message;
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size == 0) {
        return contents;
    }
    std::size_t read = 0;
    const flowget::StorageResult result =
        storage->read_at(0, reinterpret_cast<std::byte*>(contents.data()), contents.size(), read);
    EXPECT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(read, contents.size());
    return contents;
}

bool exists(const std::string& utf8) {
    std::error_code error;
    return std::filesystem::exists(utf8_path(utf8), error);
}

class ResumeStoreTest : public ::testing::Test {
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
        return std::filesystem::current_path() / "resume_store_artifacts";
    }

    // Absolute UTF-8 path, so the checks do not depend on the process working directory.
    static std::string path(const std::string& name) {
        const std::filesystem::path full = root() / utf8_path(name);
        const std::u8string bytes = full.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};

TEST_F(ResumeStoreTest, WritesTheSidecarAndLeavesNoTempFile) {
    const std::string sidecar = path(resume_sidecar_path("out.bin"));
    std::string error;

    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "{\"a\": 1}\n", error)) << error;
    EXPECT_EQ(read_file(sidecar), "{\"a\": 1}\n");
    EXPECT_FALSE(exists(sidecar + ".tmp"));
}

TEST_F(ResumeStoreTest, ReplacesAnExistingSidecar) {
    const std::string sidecar = path(resume_sidecar_path("out.bin"));
    std::string error;

    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "old", error)) << error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "new", error)) << error;
    EXPECT_EQ(read_file(sidecar), "new");
    EXPECT_FALSE(exists(sidecar + ".tmp"));
}

TEST_F(ResumeStoreTest, KeepsThePreviousSidecarWhenTheReplaceFails) {
    const std::string sidecar = path(resume_sidecar_path("out.bin"));
    std::string error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "previous", error)) << error;

    // A directory at the destination makes every platform's rename fail, which is the closest
    // reproducible stand-in for "the replace step fails".
    const std::string blocked = path(resume_sidecar_path("blocked.bin"));
    std::error_code created;
    std::filesystem::create_directories(utf8_path(blocked), created);
    ASSERT_FALSE(created) << created.message();

    error.clear();
    EXPECT_FALSE(flowget::detail::write_file_atomically(blocked, "next", error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(exists(blocked));
    EXPECT_FALSE(exists(blocked + ".tmp"));

    // Unrelated sidecar untouched.
    EXPECT_EQ(read_file(sidecar), "previous");
}

TEST_F(ResumeStoreTest, FailsWithoutCreatingTheDirectory) {
    const std::string sidecar = path("missing/out.bin.flowget");
    std::string error;

    EXPECT_FALSE(flowget::detail::write_file_atomically(sidecar, "x", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(exists(path("missing")));
}

TEST_F(ResumeStoreTest, RemovesAStaleTempFile) {
    const std::string output = path("out.bin");
    const std::string temp = path(resume_sidecar_temp_path("out.bin"));
    std::string error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(temp, "partial", error)) << error;

    ASSERT_TRUE(flowget::detail::remove_stale_sidecar_temp(output, error)) << error;
    EXPECT_FALSE(exists(temp));
    // Idempotent: a second call with nothing to delete is still success.
    EXPECT_TRUE(flowget::detail::remove_stale_sidecar_temp(output, error)) << error;
}

TEST_F(ResumeStoreTest, RejectsEmptyPaths) {
    std::string error;
    EXPECT_FALSE(flowget::detail::write_file_atomically("", "x", error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(flowget::detail::replace_file("", "a", error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ResumeStoreTest, WritesThroughANonAsciiPath) {
    const std::string output = path("İndirilenler/İndirilen_çğış.bin");
    std::error_code created;
    std::filesystem::create_directories(utf8_path(path("İndirilenler")), created);
    ASSERT_FALSE(created) << created.message();

    const std::string sidecar = path(resume_sidecar_path("İndirilenler/İndirilen_çğış.bin"));
    const std::string contents = "{\"schema_version\": 1}\n";
    std::string error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, contents, error)) << error;
    EXPECT_EQ(read_file(sidecar), contents);
    EXPECT_FALSE(exists(sidecar + ".tmp"));

    // The sidecar sits next to the output, as the engine expects.
    EXPECT_EQ(sidecar, output + ".flowget");
}

TEST_F(ResumeStoreTest, ReadsAWholeSidecarThroughIStorage) {
    const std::string sidecar = path("out.bin.flowget");
    const std::string contents = "{\"schema_version\": 1, \"url\": \"https://example.com/b\"}\n";
    {
        std::ofstream file{utf8_path(sidecar), std::ios::binary | std::ios::trunc};
        file << contents;
    }

    std::string read;
    std::string error;
    ASSERT_TRUE(read_file_utf8(sidecar, flowget::kMaxResumeStateBytes, read, error)) << error;
    EXPECT_EQ(read, contents);
}

TEST_F(ResumeStoreTest, ReadFileRejectsAnOversizedFileWithoutAllocatingIt) {
    const std::string sidecar = path("huge.bin.flowget");
    {
        std::ofstream file{utf8_path(sidecar), std::ios::binary | std::ios::trunc};
        file << std::string(256, 'x');
    }

    std::string read;
    std::string error;
    EXPECT_FALSE(read_file_utf8(sidecar, 16, read, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(read.empty());
}

TEST_F(ResumeStoreTest, ReadFileReportsAMissingFile) {
    std::string read;
    std::string error;
    EXPECT_FALSE(read_file_utf8(path("does-not-exist.flowget"), 1024, read, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ResumeStoreTest, ReplacesTheSidecarWhileAReaderHasItOpen) {
    const std::string sidecar = path("out.bin.flowget");
    std::string error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "previous", error)) << error;

    // Holding a read handle open must not block the checkpoint's rename. On Windows this only
    // works because IStorage::open(read_only) passes FILE_SHARE_READ|FILE_SHARE_WRITE|
    // FILE_SHARE_DELETE; an ifstream (or a CreateFileW without FILE_SHARE_DELETE) would make the
    // engine's own atomic rename fail with ERROR_SHARING_VIOLATION (tasks/lessons.md).
    const std::unique_ptr<IStorage> reader = flowget::make_platform_storage();
    const flowget::StorageResult opened = reader->open(sidecar, OpenMode::read_only);
    ASSERT_TRUE(opened.ok()) << opened.message;

    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, "next", error)) << error;
    std::string contents;
    ASSERT_TRUE(read_file_utf8(sidecar, flowget::kMaxResumeStateBytes, contents, error)) << error;
    EXPECT_EQ(contents, "next");
    EXPECT_FALSE(exists(sidecar + ".tmp"));
}

TEST_F(ResumeStoreTest, ReadsThroughANonAsciiPath) {
    const std::string directory = path("İndirilenler");
    std::error_code created;
    std::filesystem::create_directories(utf8_path(directory), created);
    ASSERT_FALSE(created) << created.message();

    const std::string sidecar = path(resume_sidecar_path("İndirilenler/İndirilen_çğış.bin"));
    const std::string contents = "{\"schema_version\": 1}\n";
    std::string error;
    ASSERT_TRUE(flowget::detail::write_file_atomically(sidecar, contents, error)) << error;

    std::string read;
    ASSERT_TRUE(read_file_utf8(sidecar, flowget::kMaxResumeStateBytes, read, error)) << error;
    EXPECT_EQ(read, contents);
}

} // namespace

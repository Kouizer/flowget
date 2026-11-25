#include <flowget/download.h>
#include <flowget/http_client.h>
#include <flowget/resume_state.h>
#include <flowget/single_stream_download.h>
#include <flowget/storage.h>

#include "file_replace.h"
#include "support/http_test_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

using flowget::HttpClient;
using flowget::HttpError;
using flowget::IStorage;
using flowget::OpenMode;
using flowget::SingleStreamDownloader;
using flowget::SingleStreamSettings;
using flowget::StorageError;
using flowget::StorageResult;
using flowget::test::HttpTestServer;

// Deterministic payload: one MiB of xorshift output, identical on every run, so a mismatch points
// at the download path and never at the fixture.
std::string make_payload(std::size_t size) {
    std::string payload(size, '\0');
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (char& byte : payload) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        byte = static_cast<char>(state & 0xFFU);
    }
    return payload;
}

std::filesystem::path make_temp_dir() {
    static std::atomic<int> counter{0};
    const std::filesystem::path directory = std::filesystem::current_path() / "tmp" /
                                            ("download-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

// DownloadOptions carries UTF-8 paths (S4), so the tests convert the filesystem paths they build
// themselves instead of relying on path::string(), which uses the ANSI code page on Windows.
std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string{value.begin(), value.end()};
}

std::filesystem::path from_utf8(const std::string& utf8) {
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

std::string read_file(const std::string& utf8_path) {
    std::ifstream file{from_utf8(utf8_path), std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool file_exists(const std::string& utf8_path) {
    return std::filesystem::exists(from_utf8(utf8_path));
}

std::string read_sidecar(const std::string& output_path) {
    std::string contents;
    std::string error;
    EXPECT_TRUE(flowget::detail::read_file_utf8(
        flowget::resume_sidecar_path(output_path), flowget::kMaxResumeStateBytes, contents, error))
        << error;
    return contents;
}

// Delegates everything to the real storage but fails every flush, which is how a disk that stops
// accepting data looks to the engine.
class FlushFailingStorage final : public IStorage {
public:
    explicit FlushFailingStorage(std::unique_ptr<IStorage> inner) : inner_(std::move(inner)) {}

    StorageResult open(const std::string& utf8_path, OpenMode mode) override {
        return inner_->open(utf8_path, mode);
    }

    StorageResult reopen() override {
        return inner_->reopen();
    }

    StorageResult close() override {
        return inner_->close();
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return inner_->is_open();
    }

    StorageResult preallocate(flowget::FileOffset size) override {
        return inner_->preallocate(size);
    }

    StorageResult write_at(flowget::FileOffset offset,
                           const std::byte* data,
                           std::size_t size,
                           std::size_t& written) override {
        return inner_->write_at(offset, data, size, written);
    }

    StorageResult read_at(flowget::FileOffset offset,
                          std::byte* data,
                          std::size_t size,
                          std::size_t& read) override {
        return inner_->read_at(offset, data, size, read);
    }

    StorageResult flush() override {
        return StorageResult{StorageError::flush, 1, "injected flush failure"};
    }

    StorageResult size(flowget::FileOffset& out) override {
        return inner_->size(out);
    }

private:
    std::unique_ptr<IStorage> inner_;
};

TEST(SingleStreamDownload, WritesBodyVerbatim) {
    const std::string payload = make_payload(1024 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.headers["Content-Type"] = "application/octet-stream";
        response.headers["ETag"] = "\"fixture-v1\"";
        response.headers["Accept-Ranges"] = "bytes";
        response.body = payload;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/file.bin");
    options.output_path = to_utf8(make_temp_dir() / "file.bin");

    const flowget::DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.http.response.status, 200L);
    EXPECT_EQ(summary.http.response.etag, "\"fixture-v1\"");
    EXPECT_EQ(summary.http.response.accept_ranges, "bytes");
    EXPECT_EQ(summary.http.response.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(summary.bytes_written, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SingleStreamDownload, FailsOnHttpErrorStatusAndLeavesNoFile) {
    HttpTestServer server([](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 404;
        response.body = "not here";
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/missing.bin");
    options.output_path = to_utf8(make_temp_dir() / "missing.bin");

    const flowget::DownloadSummary summary = downloader.download(options);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::http_status);
    EXPECT_EQ(summary.http.response.status, 404L);
    EXPECT_FALSE(file_exists(options.output_path));
}

TEST(SingleStreamDownload, HandlesChunkedTransferWithoutContentLength) {
    const std::string payload = make_payload(128 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.chunked = true;
        response.chunk_size = 8192;
        response.body = payload;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/chunked.bin");
    options.output_path = to_utf8(make_temp_dir() / "chunked.bin");

    const flowget::DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.http.response.content_length, -1);
    EXPECT_EQ(read_file(options.output_path), payload);
    // S5b: without Content-Length the single-stream size cannot be verified, so no sidecar is
    // written for a later run to trust.
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(SingleStreamDownload, FollowsRedirects) {
    const std::string payload = make_payload(4096);
    HttpTestServer server([&payload](const flowget::test::Request& request) {
        flowget::test::Response response;
        if (request.target == "/redirect") {
            response.status = 302;
            response.headers["Location"] = "/final.bin";
            return response;
        }
        response.status = 200;
        response.body = payload;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/redirect");
    options.output_path = to_utf8(make_temp_dir() / "final.bin");

    const flowget::DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.http.response.effective_url, server.url("/final.bin"));
    EXPECT_GE(server.request_count(), 2U);
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SingleStreamDownload, CancelsThrottledTransferAndRemovesPartialFile) {
    const std::string payload = make_payload(1024 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        response.throttle_bytes_per_second = 512 * 1024;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/slow.bin");
    options.output_path = to_utf8(make_temp_dir() / "slow.bin");

    std::atomic<bool> cancel{false};
    const flowget::DownloadSummary summary = downloader.download(
        options,
        [&cancel](const flowget::DownloadProgress& progress) {
            if (progress.bytes_written > 64 * 1024) {
                cancel.store(true);
            }
        },
        &cancel);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::aborted);
    EXPECT_LT(summary.bytes_written, static_cast<std::int64_t>(payload.size()));
    EXPECT_FALSE(file_exists(options.output_path));
    // The server sends no validator, so there is nothing a later run could verify: no sidecar.
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(SingleStreamDownload, WritesToNonAsciiPath) {
    const std::string payload = make_payload(64 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    // Nested directory that does not exist yet: the downloader must create it through the UTF-8
    // path (the old ofstream/path.string() code mangled these names on Windows).
    const std::filesystem::path directory =
        make_temp_dir() / std::filesystem::path{u8"İndirilenler"};
    const std::filesystem::path file = directory / std::filesystem::path{u8"İndirilen_çğış.bin"};

    flowget::DownloadOptions options;
    options.url = server.url("/non-ascii.bin");
    options.output_path = to_utf8(file);

    const flowget::DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.bytes_written, static_cast<std::int64_t>(payload.size()));

    // Verify through IStorage: ifstream(path) would go through the ANSI code page again and hide
    // exactly the bug this test exists for.
    const auto storage = flowget::make_platform_storage();
    const flowget::StorageResult opened =
        storage->open(options.output_path, flowget::OpenMode::read_only);
    ASSERT_TRUE(opened.ok()) << opened.message;
    flowget::FileOffset size = 0;
    ASSERT_TRUE(storage->size(size).ok());
    EXPECT_EQ(size, static_cast<flowget::FileOffset>(payload.size()));
    std::string read_back(payload.size(), '\0');
    std::size_t read = 0;
    const auto read_result =
        storage->read_at(0, reinterpret_cast<std::byte*>(read_back.data()), read_back.size(), read);
    ASSERT_TRUE(read_result.ok()) << read_result.message;
    EXPECT_EQ(read, payload.size());
    EXPECT_EQ(read_back, payload);
    EXPECT_TRUE(std::filesystem::exists(file));
}

TEST(SingleStreamDownload, PublishesPeriodicCheckpointsAndRemovesTheSidecarOnSuccess) {
    const std::string payload = make_payload(1024 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        response.headers["ETag"] = "\"checkpoint-v1\"";
        response.throttle_bytes_per_second = 256 * 1024;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/checkpoint.bin");
    options.output_path = to_utf8(make_temp_dir() / "checkpoint.bin");

    std::atomic<bool> stop{false};
    std::atomic<bool> saw_incomplete{false};
    std::jthread poller{[&options, &payload, &stop, &saw_incomplete] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::string contents;
            std::string error;
            if (flowget::detail::read_file_utf8(flowget::resume_sidecar_path(options.output_path),
                                                flowget::kMaxResumeStateBytes,
                                                contents,
                                                error)) {
                const flowget::ResumeParseResult parsed = flowget::parse_resume_state(contents);
                if (parsed.ok()) {
                    // The last timer checkpoint can land exactly on the final byte, and the
                    // sidecar disappears right after the final flush; observing that transient
                    // window is a race. An incomplete checkpoint proves the timer fired.
                    const std::int64_t done = parsed.state.bytes_done();
                    if (done > 0 && done < static_cast<std::int64_t>(payload.size())) {
                        saw_incomplete.store(true, std::memory_order_relaxed);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }};

    SingleStreamSettings settings;
    settings.checkpoint_interval = std::chrono::milliseconds{100};
    const flowget::DownloadSummary summary = downloader.download(options, {}, nullptr, settings);
    stop.store(true);
    poller.join();

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
    // A checkpoint visible while the file was still incomplete can only come from the timer.
    EXPECT_TRUE(saw_incomplete.load());
}

TEST(SingleStreamDownload, CancelKeepsTheDurablePrefixAndSidecar) {
    const std::string payload = make_payload(1024 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        response.headers["ETag"] = "\"checkpoint-v1\"";
        response.throttle_bytes_per_second = 256 * 1024;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/cancel-checkpoint.bin");
    options.output_path = to_utf8(make_temp_dir() / "cancel-checkpoint.bin");

    std::atomic<bool> cancel{false};
    SingleStreamSettings settings;
    settings.checkpoint_interval = std::chrono::milliseconds{50};
    const flowget::DownloadSummary summary = downloader.download(
        options,
        [&cancel](const flowget::DownloadProgress& progress) {
            if (progress.bytes_written > 256 * 1024) {
                cancel.store(true);
            }
        },
        &cancel,
        settings);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::aborted);
    ASSERT_TRUE(file_exists(options.output_path));
    ASSERT_TRUE(file_exists(flowget::resume_sidecar_path(options.output_path)));

    const flowget::ResumeParseResult parsed =
        flowget::parse_resume_state(read_sidecar(options.output_path));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    ASSERT_EQ(parsed.state.segments.size(), 1U);
    const flowget::ResumeSegment& segment = parsed.state.segments.front();
    EXPECT_EQ(segment.start, 0);
    EXPECT_EQ(segment.end, static_cast<std::int64_t>(payload.size()));
    EXPECT_GT(segment.done, 0);
    EXPECT_LT(segment.done, static_cast<std::int64_t>(payload.size()));

    // The file is preallocated to the full size; only the checkpointed prefix is durable data.
    const std::string partial = read_file(options.output_path);
    ASSERT_EQ(partial.size(), payload.size());
    EXPECT_EQ(partial.substr(0, static_cast<std::size_t>(segment.done)),
              payload.substr(0, static_cast<std::size_t>(segment.done)));
}

TEST(SingleStreamDownload, FlushFailureEndsWithIoAndPublishesNoSidecar) {
    const std::string payload = make_payload(512 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        response.headers["ETag"] = "\"checkpoint-v1\"";
        response.throttle_bytes_per_second = 128 * 1024;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/flush-fails.bin");
    options.output_path = to_utf8(make_temp_dir() / "flush-fails.bin");

    SingleStreamSettings settings;
    settings.storage_factory = [] {
        return std::make_unique<FlushFailingStorage>(flowget::make_platform_storage());
    };
    // Force the very first checkpoint to flush, so the failure is the checkpoint's, not only the
    // final one.
    settings.checkpoint_interval = std::chrono::milliseconds{0};
    const flowget::DownloadSummary summary = downloader.download(options, {}, nullptr, settings);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::io);
    EXPECT_NE(summary.message.find("flush"), std::string::npos) << summary.message;
    // A sidecar that claims bytes from a failed flush must never be published.
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
    EXPECT_FALSE(file_exists(options.output_path));
}

TEST(SingleStreamDownload, DetectsDroppedConnection) {
    const std::string payload = make_payload(1024 * 1024);
    HttpTestServer server([&payload](const flowget::test::Request&) {
        flowget::test::Response response;
        response.status = 200;
        response.body = payload;
        response.drop_after_bytes = 256 * 1024;
        return response;
    });

    HttpClient client;
    SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = server.url("/dropped.bin");
    options.output_path = to_utf8(make_temp_dir() / "dropped.bin");

    const flowget::DownloadSummary summary = downloader.download(options);

    EXPECT_FALSE(summary.ok());
    EXPECT_FALSE(file_exists(options.output_path));
}

} // namespace

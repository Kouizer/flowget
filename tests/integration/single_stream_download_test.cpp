#include <flowget/http_client.h>
#include <flowget/single_stream_download.h>

#include "support/http_test_server.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace {

using flowget::HttpClient;
using flowget::HttpError;
using flowget::SingleStreamDownloader;
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

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
    options.output_path = make_temp_dir() / "file.bin";

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
    options.output_path = make_temp_dir() / "missing.bin";

    const flowget::DownloadSummary summary = downloader.download(options);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::http_status);
    EXPECT_EQ(summary.http.response.status, 404L);
    EXPECT_FALSE(std::filesystem::exists(options.output_path));
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
    options.output_path = make_temp_dir() / "chunked.bin";

    const flowget::DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.http.response.content_length, -1);
    EXPECT_EQ(read_file(options.output_path), payload);
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
    options.output_path = make_temp_dir() / "final.bin";

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
    options.output_path = make_temp_dir() / "slow.bin";

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
    EXPECT_FALSE(std::filesystem::exists(options.output_path));
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
    options.output_path = make_temp_dir() / "dropped.bin";

    const flowget::DownloadSummary summary = downloader.download(options);

    EXPECT_FALSE(summary.ok());
    EXPECT_FALSE(std::filesystem::exists(options.output_path));
}

} // namespace

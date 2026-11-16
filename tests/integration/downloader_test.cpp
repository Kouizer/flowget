#include <flowget/download.h>
#include <flowget/downloader.h>
#include <flowget/http_client.h>
#include <flowget/storage.h>

#include "support/http_test_server.h"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::Downloader;
using flowget::DownloadOptions;
using flowget::DownloadProgress;
using flowget::DownloadSummary;
using flowget::HttpClient;
using flowget::HttpError;
using flowget::test::HttpTestServer;
using flowget::test::Request;
using flowget::test::Response;

constexpr std::size_t kMiB = 1024 * 1024;
// Four 2 MiB segments: enough to observe parallelism and to make every fallback scenario realistic.
constexpr std::size_t kPayloadSize = 8 * kMiB;
constexpr int kConnections = 4;

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
                                            ("segmented-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

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

std::int64_t file_size(const std::string& utf8_path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(from_utf8(utf8_path), error);
    return error ? -1 : static_cast<std::int64_t>(size);
}

struct ByteRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

// Parses the single `bytes=a-b` range requests the fixture produces.
std::optional<ByteRange> requested_range(const Request& request) {
    const auto iterator = request.headers.find("range");
    if (iterator == request.headers.end()) {
        return std::nullopt;
    }
    constexpr std::string_view kPrefix = "bytes=";
    const std::string_view value = iterator->second;
    if (value.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    std::string_view specification = value.substr(kPrefix.size());
    const std::size_t dash = specification.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t first = 0;
    std::int64_t last = 0;
    const std::string_view first_text = specification.substr(0, dash);
    const std::string_view last_text = specification.substr(dash + 1);
    const auto parsed_first =
        std::from_chars(first_text.data(), first_text.data() + first_text.size(), first);
    const auto parsed_last =
        std::from_chars(last_text.data(), last_text.data() + last_text.size(), last);
    if (parsed_first.ec != std::errc{} || parsed_last.ec != std::errc{}) {
        return std::nullopt;
    }
    return ByteRange{first, last};
}

bool is_probe_request(const Request& request) {
    const std::optional<ByteRange> range = requested_range(request);
    return range.has_value() && range->first == 0 && range->last == 0;
}

std::int64_t range_length(const ByteRange& range) {
    return range.last - range.first + 1;
}

std::string content_range_for(const ByteRange& range, std::size_t total) {
    return "bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" +
           std::to_string(total);
}

DownloadOptions make_options(const HttpTestServer& server, const std::string& file_name) {
    DownloadOptions options;
    options.url = server.url("/" + file_name);
    options.output_path = to_utf8(make_temp_dir() / file_name);
    options.connections = kConnections;
    return options;
}

TEST(SegmentedDownload, DownloadsRangesInParallel) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        // Slow enough that the four segment requests overlap instead of finishing one by one.
        response.throttle_bytes_per_second = 8 * kMiB;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "parallel.bin");
    std::vector<DownloadProgress> progress;
    const DownloadSummary summary = downloader.download(
        options, [&progress](const DownloadProgress& update) { progress.push_back(update); });

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.fallback);
    EXPECT_EQ(summary.connections, kConnections);
    EXPECT_EQ(summary.http.response.status, 206L);
    EXPECT_EQ(summary.http.response.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(summary.bytes_written, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(read_file(options.output_path), payload);

    // Proof that the ranges really ran at the same time, not one after another.
    EXPECT_GE(server.peak_concurrency(), 2);
    // HEAD + ranged probe + one request per segment.
    EXPECT_GE(server.request_count(), 2U + static_cast<std::size_t>(kConnections));
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().bytes_written, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(progress.back().total_bytes, static_cast<std::int64_t>(payload.size()));
}

TEST(SegmentedDownload, SingleConnectionSkipsProbe) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "single.bin");
    options.connections = 1;
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.fallback);
    EXPECT_EQ(summary.connections, 1);
    EXPECT_EQ(server.request_count(), 1U) << "connections=1 must not probe";
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackWhenProbeIgnoresRange) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = false;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "no-range.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "server ignored Range");
    EXPECT_EQ(summary.connections, 1);
    EXPECT_EQ(summary.http.response.status, 200L);
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackWhenSegmentAnswersWith200) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        if (is_probe_request(request)) {
            response.status = 206;
            response.headers["Content-Range"] = "bytes 0-0/" + std::to_string(payload.size());
            response.body = payload.substr(0, 1);
            return response;
        }
        // A range-capable probe, but every real range request is ignored.
        response.status = 200;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "segment-200.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_NE(summary.fallback_reason.find("instead of 206"), std::string::npos)
        << summary.fallback_reason;
    EXPECT_EQ(summary.connections, 1);
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackOnContentRangeMismatch) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        const std::optional<ByteRange> range = requested_range(request);
        if (range.has_value() && !is_probe_request(request)) {
            ByteRange shifted = *range;
            shifted.first += 1;
            shifted.last += 1;
            response.content_range_override = content_range_for(shifted, payload.size());
        }
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "bad-range.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "Content-Range mismatch");
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackOnContentLengthMismatch) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        const std::optional<ByteRange> range = requested_range(request);
        if (range.has_value() && !is_probe_request(request)) {
            response.content_length_override = std::to_string(range_length(*range) + 1);
        }
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "bad-length.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "Content-Length mismatch");
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackOnEtagMismatch) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        // The probe sees a stable ETag; the representation changes between probe and segments.
        response.headers["ETag"] =
            is_probe_request(request) || !requested_range(request).has_value() ? "\"v1\""
                                                                               : "\"v2\"";
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "etag.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "ETag mismatch");
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, FallsBackOnShortSegmentBody) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        const std::optional<ByteRange> range = requested_range(request);
        if (range.has_value() && !is_probe_request(request)) {
            // Correct Content-Range, no Content-Length, and only 16 bytes on the wire: the
            // connection closes cleanly, so libcurl reports success despite the short body.
            response.status = 206;
            response.headers["Content-Range"] = content_range_for(*range, payload.size());
            response.omit_content_length = true;
            response.body = payload.substr(static_cast<std::size_t>(range->first), 16);
            return response;
        }
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "short.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "segment byte count mismatch");
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, TransportErrorDoesNotFallBack) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        response.drop_after_bytes = 256 * 1024;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "dropped.bin");
    const DownloadSummary summary = downloader.download(options);

    EXPECT_FALSE(summary.ok());
    EXPECT_FALSE(summary.fallback);
    EXPECT_EQ(summary.http.error, HttpError::connect);
    EXPECT_FALSE(file_exists(options.output_path));
}

TEST(SegmentedDownload, CancelsAndRemovesPartialFile) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        response.throttle_bytes_per_second = 512 * 1024;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "cancel.bin");
    std::atomic<bool> cancel{false};
    const DownloadSummary summary = downloader.download(
        options,
        [&cancel](const DownloadProgress& update) {
            if (update.bytes_written > 64 * 1024) {
                cancel.store(true);
            }
        },
        &cancel);

    EXPECT_FALSE(summary.ok());
    EXPECT_EQ(summary.http.error, HttpError::aborted);
    EXPECT_FALSE(summary.fallback);
    EXPECT_FALSE(file_exists(options.output_path));
}

TEST(SegmentedDownload, EmptyResourceCreatesEmptyFileWithoutSegments) {
    HttpTestServer server([](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body.clear();
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "empty.bin");
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.fallback);
    EXPECT_EQ(summary.connections, 0);
    EXPECT_EQ(summary.bytes_written, 0);
    EXPECT_EQ(summary.http.response.content_length, 0);
    ASSERT_TRUE(file_exists(options.output_path));
    EXPECT_EQ(file_size(options.output_path), 0);
}

TEST(SegmentedDownload, OverwritesExistingFile) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "overwrite.bin");
    {
        std::ofstream stale{from_utf8(options.output_path), std::ios::binary};
        stale << std::string(10 * kMiB, 'x');
    }
    ASSERT_EQ(file_size(options.output_path), static_cast<std::int64_t>(10 * kMiB));

    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.bytes_written, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(file_size(options.output_path), static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(SegmentedDownload, WritesToNonAsciiPath) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    HttpClient client;
    Downloader downloader{client};

    DownloadOptions options = make_options(server, "non-ascii.bin");
    const std::filesystem::path directory =
        make_temp_dir() / std::filesystem::path{u8"İndirilenler"};
    const std::filesystem::path file = directory / std::filesystem::path{u8"İndirilen_çğış.bin"};
    options.output_path = to_utf8(file);

    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(summary.bytes_written, static_cast<std::int64_t>(payload.size()));
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(read_file(options.output_path), payload);
}

} // namespace

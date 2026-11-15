#include <flowget/http_client.h>
#include <flowget/probe.h>

#include "support/http_test_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using flowget::HttpClient;
using flowget::HttpError;
using flowget::ProbeOptions;
using flowget::ProbeResult;
using flowget::RangeSupport;
using flowget::test::HttpTestServer;
using flowget::test::Request;
using flowget::test::Response;

// Same deterministic xorshift payload as the download tests: a mismatch always points at the probe
// path, never at the fixture.
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

ProbeOptions options_for(const HttpTestServer& server, std::string_view path) {
    ProbeOptions options;
    options.url = server.url(path);
    return options;
}

TEST(Probe, ReportsRangeSupportAndMetadata) {
    const std::string payload = make_payload(128 * 1024);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.body = payload;
        response.honor_range = true;
        response.headers["Content-Type"] = "application/octet-stream";
        response.headers["ETag"] = "\"fixture-v1\"";
        response.headers["Last-Modified"] = "Wed, 21 Oct 2015 07:28:00 GMT";
        response.headers["Accept-Ranges"] = "bytes";
        response.headers["Content-Disposition"] = "attachment; filename=\"fixture.bin\"";
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/file.bin"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.status, 206L);
    EXPECT_EQ(result.range_support, RangeSupport::supported);
    EXPECT_EQ(result.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(result.etag, "\"fixture-v1\"");
    EXPECT_EQ(result.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT");
    EXPECT_EQ(result.accept_ranges, "bytes");
    EXPECT_EQ(result.content_type, "application/octet-stream");
    EXPECT_EQ(result.content_disposition, "attachment; filename=\"fixture.bin\"");
    EXPECT_EQ(result.suggested_filename, "fixture.bin");
    EXPECT_EQ(result.effective_url, server.url("/file.bin"));
    EXPECT_TRUE(result.head_used);
}

TEST(Probe, DerivesFilenameFromUrlWhenHeaderIsAbsent) {
    const std::string payload = make_payload(4096);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.body = payload;
        response.honor_range = true;
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/media/clip.mp4"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.suggested_filename, "clip.mp4");
}

TEST(Probe, DetectsServersThatIgnoreRange) {
    // 256 KiB is larger than the probe's body limit, so this also covers the deliberate abort: the
    // server starts streaming the whole file, the sink stops it after 64 KiB, and the probe must
    // still report a usable result.
    const std::string payload = make_payload(256 * 1024);
    std::atomic<int> ranged_requests{0};
    HttpTestServer server([&](const Request& request) {
        Response response;
        response.body = payload;
        if (request.headers.count("range") != 0U) {
            ranged_requests.fetch_add(1);
        }
        return response; // honor_range defaults to false: Range is ignored, 200 + full body
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/full.bin"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.status, 200L);
    EXPECT_EQ(result.range_support, RangeSupport::ignored);
    EXPECT_EQ(result.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(ranged_requests.load(), 1);
}

TEST(Probe, ReportsUnsatisfiableRangeForEmptyResource) {
    HttpTestServer server([](const Request&) {
        Response response;
        response.honor_range = true; // empty body: any range starts past the end
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/empty.bin"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.status, 416L);
    EXPECT_EQ(result.range_support, RangeSupport::unsatisfiable);
    EXPECT_EQ(result.content_length, 0);
}

TEST(Probe, FailsOnHttpErrorStatus) {
    HttpTestServer server([](const Request&) {
        Response response;
        response.status = 404;
        response.body = "not here";
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/missing.bin"));

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, HttpError::http_status);
    EXPECT_EQ(result.status, 404L);
}

TEST(Probe, FallsBackWhenHeadIsForbidden) {
    // Signed URLs commonly allow GET only and answer 403 to HEAD.
    const std::string payload = make_payload(32 * 1024);
    std::atomic<int> head_requests{0};
    HttpTestServer server([&](const Request& request) {
        Response response;
        if (request.method == "HEAD") {
            head_requests.fetch_add(1);
            response.status = 403;
            return response;
        }
        response.body = payload;
        response.honor_range = true;
        response.headers["ETag"] = "\"signed-v1\"";
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/signed.bin"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(head_requests.load(), 1);
    EXPECT_FALSE(result.head_used);
    EXPECT_EQ(result.status, 206L);
    EXPECT_EQ(result.range_support, RangeSupport::supported);
    EXPECT_EQ(result.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(result.etag, "\"signed-v1\"");
}

TEST(Probe, FallsBackWhenHeadStalls) {
    const std::string payload = make_payload(16 * 1024);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        if (request.method == "HEAD") {
            response.stall = true; // accept the request, never answer
            return response;
        }
        response.body = payload;
        response.honor_range = true;
        return response;
    });

    ProbeOptions options = options_for(server, "/slow-head.bin");
    options.head_timeout_ms = 200;

    HttpClient client;
    const auto started = std::chrono::steady_clock::now();
    const ProbeResult result = flowget::probe(client, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_FALSE(result.head_used);
    EXPECT_EQ(result.range_support, RangeSupport::supported);
    EXPECT_EQ(result.content_length, static_cast<std::int64_t>(payload.size()));
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(Probe, SkipsHeadWhenDisabled) {
    const std::string payload = make_payload(8192);
    std::atomic<int> head_requests{0};
    HttpTestServer server([&](const Request& request) {
        Response response;
        if (request.method == "HEAD") {
            head_requests.fetch_add(1);
        }
        response.body = payload;
        response.honor_range = true;
        return response;
    });

    ProbeOptions options = options_for(server, "/no-head.bin");
    options.head_timeout_ms = 0;

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options);

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(head_requests.load(), 0);
    EXPECT_FALSE(result.head_used);
    EXPECT_EQ(result.content_length, static_cast<std::int64_t>(payload.size()));
}

TEST(Probe, FollowsRedirects) {
    const std::string payload = make_payload(12 * 1024);
    HttpTestServer server([&payload](const Request& request) {
        Response response;
        if (request.target == "/redirect") {
            response.status = 302;
            response.headers["Location"] = "/final.bin";
            return response;
        }
        response.body = payload;
        response.honor_range = true;
        return response;
    });

    HttpClient client;
    const ProbeResult result = flowget::probe(client, options_for(server, "/redirect"));

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(result.status, 206L);
    EXPECT_EQ(result.effective_url, server.url("/final.bin"));
    EXPECT_EQ(result.range_support, RangeSupport::supported);
}

TEST(Probe, SeesEtagChangeBetweenProbes) {
    const std::string payload = make_payload(8 * 1024);
    std::atomic<int> revision{1};
    HttpTestServer server([&](const Request&) {
        Response response;
        response.body = payload;
        response.honor_range = true;
        response.headers["ETag"] =
            "\"v" + std::to_string(revision.load(std::memory_order_relaxed)) + "\"";
        return response;
    });

    HttpClient client;

    const ProbeResult first = flowget::probe(client, options_for(server, "/file.bin"));
    ASSERT_TRUE(first.ok()) << first.message;
    EXPECT_EQ(first.etag, "\"v1\"");

    revision.store(2);
    const ProbeResult second = flowget::probe(client, options_for(server, "/file.bin"));
    ASSERT_TRUE(second.ok()) << second.message;
    EXPECT_EQ(second.etag, "\"v2\"");
}

} // namespace

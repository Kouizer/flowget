#include <flowget/download.h>
#include <flowget/downloader.h>
#include <flowget/http_client.h>
#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "support/http_test_server.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::Downloader;
using flowget::DownloadOptions;
using flowget::DownloadProgress;
using flowget::DownloadSummary;
using flowget::HttpClient;
using flowget::HttpError;
using flowget::kMaxResumeStateBytes;
using flowget::parse_resume_state;
using flowget::ResumeParseResult;
using flowget::ResumeSegment;
using flowget::ResumeState;
using flowget::serialize_resume_state;
using flowget::test::HttpTestServer;
using flowget::test::Request;
using flowget::test::Response;

constexpr std::size_t kMiB = 1024 * 1024;
// Four 2 MiB segments: the same shape the Downloader tests use, and big enough that a 40% cancel
// leaves several partly-written ranges behind.
constexpr std::size_t kPayloadSize = 8 * kMiB;
constexpr int kConnections = 4;
constexpr const char* kEtag = "\"stable-v1\"";
constexpr const char* kLastModified = "Tue, 17 Nov 2026 10:00:00 GMT";

std::string make_payload(std::size_t size, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::string payload(size, '\0');
    std::uint64_t state = seed;
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
                                            ("resume-" + std::to_string(counter.fetch_add(1)));
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

void write_file(const std::string& utf8_path, std::string_view contents) {
    std::ofstream file{from_utf8(utf8_path), std::ios::binary | std::ios::trunc};
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void preallocate_file(const std::string& utf8_path, std::int64_t size) {
    {
        std::ofstream file{from_utf8(utf8_path), std::ios::binary | std::ios::trunc};
    }
    std::error_code error;
    std::filesystem::resize_file(from_utf8(utf8_path), static_cast<std::uintmax_t>(size), error);
    ASSERT_FALSE(error) << error.message();
}

// Writes bytes at offset 0 without truncating, so a preallocated file keeps its announced length.
void write_prefix(const std::string& utf8_path, std::string_view bytes) {
    std::fstream file{from_utf8(utf8_path), std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file) << "cannot open " << utf8_path;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
}

bool file_exists(const std::string& utf8_path) {
    return std::filesystem::exists(from_utf8(utf8_path));
}

std::string read_sidecar(const std::string& output_path) {
    return read_file(flowget::resume_sidecar_path(output_path));
}

void write_sidecar(const std::string& output_path, std::string_view contents) {
    write_file(flowget::resume_sidecar_path(output_path), contents);
}

std::string strip_json_line(std::string text, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const std::size_t position = text.find(needle);
    if (position == std::string::npos) {
        return text;
    }
    const std::size_t line_start = text.rfind('\n', position) + 1;
    const std::size_t line_end = text.find('\n', position);
    text.erase(line_start, line_end - line_start + 1);
    return text;
}

ResumeState partial_state(const std::string& url,
                          std::int64_t total,
                          std::int64_t done_first,
                          std::string etag = std::string{kEtag},
                          std::string last_modified = {}) {
    ResumeState state;
    state.url = url;
    state.total_bytes = total;
    state.etag = std::move(etag);
    state.last_modified = std::move(last_modified);
    const std::int64_t half = total / 2;
    state.segments = {ResumeSegment{0, half, done_first}, ResumeSegment{half, total, 0}};
    return state;
}

struct CapturedRequests {
    void add(const Request& request) {
        const std::lock_guard<std::mutex> lock{mutex};
        requests.push_back(request);
    }

    [[nodiscard]] std::vector<Request> snapshot() const {
        const std::lock_guard<std::mutex> lock{mutex};
        return requests;
    }

    void clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        requests.clear();
    }

    mutable std::mutex mutex;
    std::vector<Request> requests;
};

struct ByteRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

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
    const std::string_view specification = value.substr(kPrefix.size());
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

Response ranged_response(const std::string& payload) {
    Response response;
    response.status = 200;
    response.honor_range = true;
    response.body = payload;
    response.headers["ETag"] = kEtag;
    response.headers["Last-Modified"] = kLastModified;
    return response;
}

DownloadOptions make_options(const HttpTestServer& server, const std::string& name) {
    DownloadOptions options;
    options.url = server.url("/" + name);
    options.output_path = to_utf8(make_temp_dir() / name);
    options.connections = kConnections;
    return options;
}

// Cancels once the progress callback reports the requested share of the file.
std::function<void(const DownloadProgress&)>
cancel_after(std::atomic<bool>& cancel, std::int64_t total, double fraction) {
    const auto threshold = static_cast<std::int64_t>(static_cast<double>(total) * fraction);
    return [&cancel, threshold](const DownloadProgress& update) {
        if (update.bytes_written >= threshold) {
            cancel.store(true);
        }
    };
}

DownloadSummary cancel_first_run(const DownloadOptions& options, std::int64_t total) {
    HttpClient client;
    Downloader downloader{client};
    std::atomic<bool> cancel{false};
    const DownloadSummary summary =
        downloader.download(options, cancel_after(cancel, total, 0.4), &cancel);
    EXPECT_EQ(summary.http.error, HttpError::aborted) << summary.message;
    EXPECT_TRUE(file_exists(options.output_path));
    return summary;
}

TEST(ResumeDownload, ContinuesFromACheckpointAfterCancellation) {
    const std::string payload = make_payload(kPayloadSize);
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        Response response = ranged_response(payload);
        response.throttle_bytes_per_second = 2 * kMiB;
        return response;
    });

    DownloadOptions options = make_options(server, "continue.bin");
    std::vector<std::int64_t> notices;
    options.on_resume = [&notices](std::int64_t bytes) { notices.push_back(bytes); };

    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front(), 0);

    const std::string sidecar = flowget::resume_sidecar_path(options.output_path);
    ASSERT_TRUE(file_exists(sidecar));
    const ResumeParseResult parsed = parse_resume_state(read_sidecar(options.output_path));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const std::int64_t durable = parsed.state.bytes_done();
    ASSERT_GT(durable, 0);
    ASSERT_LT(durable, static_cast<std::int64_t>(payload.size()));

    // Only the incomplete suffix of every range may be requested, starting at its checkpoint.
    std::vector<std::int64_t> expected_starts;
    for (const ResumeSegment& segment : parsed.state.segments) {
        if (segment.done < segment.length()) {
            expected_starts.push_back(segment.start + segment.done);
        }
    }
    captured.clear();

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary second = downloader.download(options);

    ASSERT_TRUE(second.ok()) << second.message;
    EXPECT_TRUE(second.resumed);
    EXPECT_EQ(second.resumed_from_bytes, durable);
    EXPECT_TRUE(second.resume_note.empty()) << second.resume_note;
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(sidecar));
    ASSERT_EQ(notices.size(), 2U);
    EXPECT_EQ(notices.back(), durable);

    std::vector<std::int64_t> requested_starts;
    for (const Request& request : captured.snapshot()) {
        const std::optional<ByteRange> range = requested_range(request);
        if (!range.has_value() || is_probe_request(request)) {
            continue;
        }
        requested_starts.push_back(range->first);
        const auto if_range = request.headers.find("if-range");
        ASSERT_NE(if_range, request.headers.end());
        EXPECT_EQ(if_range->second, kEtag);
    }
    std::sort(requested_starts.begin(), requested_starts.end());
    std::sort(expected_starts.begin(), expected_starts.end());
    EXPECT_EQ(requested_starts, expected_starts);
}

TEST(ResumeDownload, SingleStreamResumesItsOwnSidecar) {
    const std::string payload = make_payload(2 * kMiB);
    const std::int64_t total = static_cast<std::int64_t>(payload.size());
    const std::int64_t durable = 700 * 1024;
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        return ranged_response(payload);
    });

    DownloadOptions options = make_options(server, "single-resume.bin");
    options.connections = 1;
    std::vector<std::int64_t> notices;
    options.on_resume = [&notices](std::int64_t bytes) { notices.push_back(bytes); };

    preallocate_file(options.output_path, total);
    // The file holds an unflushed tail beyond the sidecar's done; only `done` may be trusted.
    write_prefix(options.output_path, std::string_view{payload}.substr(0, 900 * 1024));
    ResumeState state;
    state.url = options.url;
    state.total_bytes = total;
    state.etag = kEtag;
    state.segments = {ResumeSegment{0, total, durable}};
    write_sidecar(options.output_path, serialize_resume_state(state));

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.resumed);
    EXPECT_EQ(summary.resumed_from_bytes, durable);
    EXPECT_TRUE(summary.resume_note.empty()) << summary.resume_note;
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));

    const std::vector<Request> requests = captured.snapshot();
    ASSERT_EQ(requests.size(), 1U);
    const auto range = requests.front().headers.find("range");
    ASSERT_NE(range, requests.front().headers.end());
    EXPECT_EQ(range->second, "bytes=" + std::to_string(durable) + "-");
    const auto if_range = requests.front().headers.find("if-range");
    ASSERT_NE(if_range, requests.front().headers.end());
    EXPECT_EQ(if_range->second, kEtag);

    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front(), durable);
}

TEST(ResumeDownload, PublishesPeriodicCheckpointsDuringALongTransfer) {
    const std::string payload = make_payload(4 * kMiB);
    HttpTestServer server([&payload](const Request&) {
        Response response = ranged_response(payload);
        // Two 2 MiB ranges at 768 KiB/s each: the transfer outlives the fixed 2 s interval.
        response.throttle_bytes_per_second = 768 * 1024;
        return response;
    });

    DownloadOptions options = make_options(server, "periodic.bin");
    options.connections = 2;

    std::atomic<bool> stop{false};
    std::atomic<bool> saw_incomplete{false};
    std::jthread poller{[&options, &payload, &stop, &saw_incomplete] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::string sidecar = flowget::resume_sidecar_path(options.output_path);
            if (file_exists(sidecar)) {
                const ResumeParseResult parsed =
                    parse_resume_state(read_sidecar(options.output_path));
                if (parsed.ok()) {
                    // The engine also publishes a full-size checkpoint when the last segment
                    // finishes and removes it after the final flush; whether the poller observes
                    // that transient window is a race. Only an incomplete checkpoint proves the
                    // timer fired mid-transfer.
                    const std::int64_t done = parsed.state.bytes_done();
                    if (done > 0 && done < static_cast<std::int64_t>(payload.size())) {
                        saw_incomplete.store(true, std::memory_order_relaxed);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }};

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);
    stop.store(true);
    poller.join();

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
    // A checkpoint visible while the file was still incomplete can only come from the timer.
    EXPECT_TRUE(saw_incomplete.load());
}

TEST(ResumeDownload, RedirectTargetMayChangeWhileTheOriginalUrlResumes) {
    const std::string payload = make_payload(kPayloadSize);
    std::atomic<int> version{1};
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured, &version](const Request& request) {
        captured.add(request);
        if (request.target == "/start") {
            Response redirect;
            redirect.status = 302;
            redirect.headers["Location"] = version.load() == 1 ? "/cdn/v1" : "/cdn/v2";
            return redirect;
        }
        Response response = ranged_response(payload);
        response.throttle_bytes_per_second = 2 * kMiB;
        return response;
    });

    DownloadOptions options;
    options.url = server.url("/start");
    options.output_path = to_utf8(make_temp_dir() / "redirect.bin");
    options.connections = kConnections;

    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    const ResumeParseResult parsed = parse_resume_state(read_sidecar(options.output_path));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    // The sidecar identifies the user's request, not the CDN hop of the first run.
    EXPECT_EQ(parsed.state.url, options.url);

    version.store(2);
    captured.clear();

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.resumed);
    EXPECT_GT(summary.resumed_from_bytes, 0);
    EXPECT_EQ(read_file(options.output_path), payload);

    bool saw_segment = false;
    for (const Request& request : captured.snapshot()) {
        EXPECT_NE(request.target, "/cdn/v1");
        const std::optional<ByteRange> range = requested_range(request);
        if (!range.has_value() || is_probe_request(request)) {
            continue;
        }
        saw_segment = true;
        EXPECT_EQ(request.target, "/cdn/v2");
        const auto if_range = request.headers.find("if-range");
        ASSERT_NE(if_range, request.headers.end());
        EXPECT_EQ(if_range->second, kEtag);
    }
    EXPECT_TRUE(saw_segment);
}

TEST(ResumeDownload, RestartsFromZeroWhenTheEtagChanges) {
    const std::string first_payload = make_payload(kPayloadSize);
    const std::string second_payload = make_payload(kPayloadSize, 0x2545f4914f6cdd1dULL);
    std::atomic<int> version{1};
    HttpTestServer server([&](const Request&) {
        Response response = ranged_response(version.load() == 1 ? first_payload : second_payload);
        response.headers["ETag"] = version.load() == 1 ? kEtag : "\"stable-v2\"";
        if (version.load() == 1) {
            response.throttle_bytes_per_second = 2 * kMiB;
        }
        return response;
    });

    DownloadOptions options = make_options(server, "etag-change.bin");
    cancel_first_run(options, static_cast<std::int64_t>(first_payload.size()));

    version.store(2);
    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.resumed);
    EXPECT_EQ(summary.resumed_from_bytes, 0);
    EXPECT_NE(summary.resume_note.find("validator"), std::string::npos) << summary.resume_note;
    EXPECT_EQ(read_file(options.output_path), second_payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(ResumeDownload, UsesLastModifiedForIfRangeWhenTheEtagIsWeak) {
    const std::string payload = make_payload(kPayloadSize);
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        Response response = ranged_response(payload);
        response.headers["ETag"] = "W/\"weak-v1\"";
        response.throttle_bytes_per_second = 2 * kMiB;
        return response;
    });

    DownloadOptions options = make_options(server, "weak-etag.bin");
    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    captured.clear();

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.resumed);
    EXPECT_EQ(read_file(options.output_path), payload);

    bool saw_segment = false;
    for (const Request& request : captured.snapshot()) {
        const std::optional<ByteRange> range = requested_range(request);
        if (!range.has_value() || is_probe_request(request)) {
            continue;
        }
        saw_segment = true;
        // A weak ETag must never be used for If-Range (RFC 9110 13.1.5).
        const auto if_range = request.headers.find("if-range");
        ASSERT_NE(if_range, request.headers.end());
        EXPECT_EQ(if_range->second, kLastModified);
    }
    EXPECT_TRUE(saw_segment);
}

TEST(ResumeDownload, WithoutAValidatorNoCheckpointIsWrittenAndTheNextRunStartsFresh) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        response.throttle_bytes_per_second = 2 * kMiB;
        return response;
    });

    DownloadOptions options = make_options(server, "no-validator.bin");
    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.resumed);
    EXPECT_EQ(read_file(options.output_path), payload);
}

TEST(ResumeDownload, DiscardsCorruptSidecarsAndDownloadsFresh) {
    const std::string payload = make_payload(kPayloadSize);
    const auto total = static_cast<std::int64_t>(payload.size());
    HttpTestServer server([&payload](const Request&) { return ranged_response(payload); });

    const std::string url = server.url("/corrupt.bin");
    ResumeState valid = partial_state(url, total, kMiB);

    ResumeState wrong_version = valid;
    wrong_version.schema_version = 99;

    ResumeState not_contiguous = valid;
    not_contiguous.segments = {ResumeSegment{0, kMiB / 2, 0}, ResumeSegment{kMiB, total, 0}};

    ResumeState done_out_of_range = valid;
    done_out_of_range.segments = {ResumeSegment{0, total, total + 1}};

    const std::vector<std::string> variants = {
        "{ \"schema_version\": 1,",
        serialize_resume_state(wrong_version),
        strip_json_line(serialize_resume_state(valid), "etag"),
        serialize_resume_state(not_contiguous),
        serialize_resume_state(done_out_of_range),
    };

    for (std::size_t index = 0; index < variants.size(); ++index) {
        SCOPED_TRACE("variant " + std::to_string(index));
        DownloadOptions options;
        options.url = url;
        options.output_path =
            to_utf8(make_temp_dir() / ("corrupt-" + std::to_string(index) + ".bin"));
        options.connections = kConnections;
        preallocate_file(options.output_path, total);
        write_sidecar(options.output_path, variants[index]);

        HttpClient client;
        Downloader downloader{client};
        const DownloadSummary summary = downloader.download(options);

        ASSERT_TRUE(summary.ok()) << summary.message;
        EXPECT_FALSE(summary.resumed);
        EXPECT_FALSE(summary.resume_note.empty());
        EXPECT_EQ(read_file(options.output_path), payload);
        EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
    }
}

TEST(ResumeDownload, RestartsWhenThePartialFileIsMissingOrHasTheWrongSize) {
    const std::string payload = make_payload(kPayloadSize);
    const auto total = static_cast<std::int64_t>(payload.size());
    HttpTestServer server([&payload](const Request&) { return ranged_response(payload); });
    const std::string url = server.url("/file-check.bin");
    const std::string valid = serialize_resume_state(partial_state(url, total, kMiB));

    for (const bool create_file : {false, true}) {
        SCOPED_TRACE(create_file ? "wrong size" : "missing");
        DownloadOptions options;
        options.url = url;
        options.output_path =
            to_utf8(make_temp_dir() / (create_file ? "wrong-size.bin" : "missing.bin"));
        options.connections = kConnections;
        if (create_file) {
            preallocate_file(options.output_path, total / 2);
        }
        write_sidecar(options.output_path, valid);

        HttpClient client;
        Downloader downloader{client};
        const DownloadSummary summary = downloader.download(options);

        ASSERT_TRUE(summary.ok()) << summary.message;
        EXPECT_FALSE(summary.resumed);
        EXPECT_NE(summary.resume_note.find("file"), std::string::npos) << summary.resume_note;
        EXPECT_EQ(read_file(options.output_path), payload);
        EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
    }
}

TEST(ResumeDownload, AlreadyCompleteOutputNeedsNoRequest) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([](const Request&) {
        Response response;
        response.status = 500;
        return response;
    });

    DownloadOptions options = make_options(server, "complete.bin");
    write_file(options.output_path, payload);

    ResumeState state;
    state.url = options.url;
    state.total_bytes = static_cast<std::int64_t>(payload.size());
    state.etag = kEtag;
    state.segments = {ResumeSegment{0, state.total_bytes, state.total_bytes}};
    write_sidecar(options.output_path, serialize_resume_state(state));

    std::vector<std::int64_t> notices;
    options.on_resume = [&notices](std::int64_t bytes) { notices.push_back(bytes); };

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.resumed);
    EXPECT_EQ(summary.resumed_from_bytes, state.total_bytes);
    EXPECT_EQ(summary.bytes_written, state.total_bytes);
    EXPECT_EQ(summary.resume_note, "already complete");
    EXPECT_EQ(server.request_count(), 0U);
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front(), state.total_bytes);
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(ResumeDownload, ResumeFalseDeletesTheSidecarAndStartsOver) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) {
        Response response = ranged_response(payload);
        response.throttle_bytes_per_second = 2 * kMiB;
        return response;
    });

    DownloadOptions options = make_options(server, "no-resume.bin");
    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    ASSERT_TRUE(file_exists(flowget::resume_sidecar_path(options.output_path)));

    options.resume = false;
    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_FALSE(summary.resumed);
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(ResumeDownload, FallsBackToASingleStreamOnContentLengthMismatch) {
    const std::string payload = make_payload(kPayloadSize);
    std::atomic<bool> lie{false};
    HttpTestServer server([&payload, &lie](const Request& request) {
        Response response = ranged_response(payload);
        response.throttle_bytes_per_second = lie.load() ? 0 : 2 * kMiB;
        const std::optional<ByteRange> range = requested_range(request);
        if (lie.load() && range.has_value() && !is_probe_request(request)) {
            response.content_length_override = std::to_string(range_length(*range) + 1);
        }
        return response;
    });

    DownloadOptions options = make_options(server, "bad-length.bin");
    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    lie.store(true);

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_EQ(summary.fallback_reason, "Content-Length mismatch");
    EXPECT_FALSE(summary.resumed);
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

TEST(ResumeDownload, AChangedRepresentationCostsOneReplanThenASingleStream) {
    const std::string payload = make_payload(kPayloadSize);
    std::atomic<bool> reject_resume{false};
    std::atomic<int> rejected{0};
    HttpTestServer server([&payload, &reject_resume, &rejected](const Request& request) {
        if (reject_resume.load() && request.headers.contains("if-range") &&
            !is_probe_request(request)) {
            // The resource changed: If-Range makes the server answer with the whole new body.
            rejected.fetch_add(1);
            Response response;
            response.status = 200;
            response.body = payload;
            return response;
        }
        Response response = ranged_response(payload);
        response.throttle_bytes_per_second = reject_resume.load() ? 0 : 2 * kMiB;
        return response;
    });

    DownloadOptions options = make_options(server, "changed.bin");
    cancel_first_run(options, static_cast<std::int64_t>(payload.size()));
    reject_resume.store(true);

    HttpClient client;
    Downloader downloader{client};
    const DownloadSummary summary = downloader.download(options);

    ASSERT_TRUE(summary.ok()) << summary.message;
    EXPECT_TRUE(summary.fallback);
    EXPECT_FALSE(summary.resumed);
    EXPECT_NE(summary.resume_note.find("changed while resuming"), std::string::npos)
        << summary.resume_note;
    // The resumed attempt and the fresh plan both get a 200; only then does the single stream run.
    EXPECT_GE(rejected.load(), 2);
    EXPECT_EQ(read_file(options.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(options.output_path)));
}

} // namespace

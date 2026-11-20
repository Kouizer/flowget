#include <flowget/download.h>
#include <flowget/probe.h>
#include <flowget/resume_state.h>
#include <flowget/segment_planner.h>
#include <flowget/storage.h>

#include "file_replace.h"
#include "segmented_download.h"
#include "support/http_test_server.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using flowget::DownloadOptions;
using flowget::DownloadProgress;
using flowget::FileOffset;
using flowget::HttpError;
using flowget::IStorage;
using flowget::OpenMode;
using flowget::ProbeResult;
using flowget::RangeSupport;
using flowget::ResumeParseResult;
using flowget::Segment;
using flowget::StorageError;
using flowget::StorageResult;
using flowget::detail::run_segmented;
using flowget::detail::SegmentedOutcome;
using flowget::detail::SegmentedSettings;
using flowget::detail::SegmentTask;
using flowget::test::HttpTestServer;
using flowget::test::Request;
using flowget::test::Response;

constexpr std::int64_t kMiB = 1024 * 1024;

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
    const std::filesystem::path directory =
        std::filesystem::current_path() / "tmp" /
        ("segmented-engine-" + std::to_string(counter.fetch_add(1)));
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

void write_prefix(const std::string& utf8_path, const std::string& payload, std::int64_t size) {
    std::ofstream file{from_utf8(utf8_path), std::ios::binary | std::ios::trunc};
    ASSERT_GE(static_cast<std::int64_t>(payload.size()), size);
    file.write(payload.data(), size);
}

bool file_exists(const std::string& utf8_path) {
    return std::filesystem::exists(from_utf8(utf8_path));
}

ProbeResult make_probe(const std::string& url,
                       std::int64_t total,
                       std::string etag,
                       std::string last_modified) {
    ProbeResult probe;
    probe.status = 206;
    probe.content_length = total;
    probe.range_support = RangeSupport::supported;
    probe.effective_url = url;
    probe.etag = std::move(etag);
    probe.last_modified = std::move(last_modified);
    return probe;
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

    mutable std::mutex mutex;
    std::vector<Request> requests;
};

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

    StorageResult preallocate(FileOffset size) override {
        return inner_->preallocate(size);
    }

    StorageResult write_at(FileOffset offset,
                           const std::byte* data,
                           std::size_t size,
                           std::size_t& written) override {
        return inner_->write_at(offset, data, size, written);
    }

    StorageResult
    read_at(FileOffset offset, std::byte* data, std::size_t size, std::size_t& read) override {
        return inner_->read_at(offset, data, size, read);
    }

    StorageResult flush() override {
        return StorageResult{StorageError::flush, 1, "injected flush failure"};
    }

    StorageResult size(FileOffset& out) override {
        return inner_->size(out);
    }

private:
    std::unique_ptr<IStorage> inner_;
};

class SegmentedEngineTest : public ::testing::Test {
protected:
    DownloadOptions options(const HttpTestServer& server, const std::string& name) {
        DownloadOptions result;
        result.url = server.url("/" + name);
        result.output_path = to_utf8(make_temp_dir() / name);
        return result;
    }
};

TEST_F(SegmentedEngineTest, ResumesFromTaskProgressAndSkipsFinishedRanges) {
    const std::string payload = make_payload(static_cast<std::size_t>(4 * kMiB));
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    DownloadOptions download = options(server, "resume.bin");
    const ProbeResult probe = make_probe(download.url, 4 * kMiB, "\"strong\"", "");

    // Bytes [0, 512 KiB) and the whole second range are already durable. The file is sparse: the
    // zeros elsewhere are what the engine is expected to overwrite.
    {
        std::ofstream file{from_utf8(download.output_path), std::ios::binary | std::ios::trunc};
    }
    std::error_code resized;
    std::filesystem::resize_file(from_utf8(download.output_path), 2 * kMiB, resized);
    ASSERT_FALSE(resized) << resized.message();
    {
        std::fstream file{from_utf8(download.output_path),
                          std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(0);
        file.write(payload.data(), 512 * 1024);
        file.seekp(kMiB);
        file.write(payload.data() + kMiB, kMiB);
    }

    const std::vector<SegmentTask> tasks = {
        {Segment{0, kMiB}, 512 * 1024},
        {Segment{kMiB, kMiB}, kMiB},
        {Segment{2 * kMiB, kMiB}, 0},
        {Segment{3 * kMiB, kMiB}, 0},
    };

    const SegmentedOutcome outcome =
        run_segmented(download, probe, tasks, nullptr, nullptr, SegmentedSettings{});
    ASSERT_TRUE(outcome.summary.ok()) << outcome.summary.message;
    ASSERT_TRUE(outcome.fallback_reason.empty()) << outcome.fallback_reason;
    EXPECT_EQ(outcome.summary.connections, 3);
    EXPECT_EQ(outcome.summary.bytes_written, 4 * kMiB);
    EXPECT_EQ(read_file(download.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(download.output_path)));

    const std::vector<Request> requests = captured.snapshot();
    ASSERT_EQ(requests.size(), 3U);
    bool saw_resumed_range = false;
    for (const Request& request : requests) {
        const auto range = request.headers.find("range");
        ASSERT_NE(range, request.headers.end());
        // The finished second range must not be requested again.
        EXPECT_EQ(range->second.find("bytes=1048576-"), std::string::npos) << range->second;
        saw_resumed_range =
            saw_resumed_range || range->second.find("bytes=524288-1048575") != std::string::npos;
        const auto if_range = request.headers.find("if-range");
        ASSERT_NE(if_range, request.headers.end());
        EXPECT_EQ(if_range->second, "\"strong\"");
    }
    EXPECT_TRUE(saw_resumed_range);
}

TEST_F(SegmentedEngineTest, WeakEtagFallsBackToLastModifiedForIfRange) {
    const std::string payload = make_payload(static_cast<std::size_t>(2 * kMiB));
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    DownloadOptions download = options(server, "weak.bin");
    constexpr std::string_view kLastModified = "Tue, 17 Nov 2026 10:00:00 GMT";
    const ProbeResult probe =
        make_probe(download.url, 2 * kMiB, "W/\"weak\"", std::string{kLastModified});
    const std::vector<SegmentTask> tasks = {{Segment{0, kMiB}, 0}, {Segment{kMiB, kMiB}, 0}};

    const SegmentedOutcome outcome =
        run_segmented(download, probe, tasks, nullptr, nullptr, SegmentedSettings{});
    ASSERT_TRUE(outcome.summary.ok()) << outcome.summary.message;
    EXPECT_EQ(read_file(download.output_path), payload);

    const std::vector<Request> requests = captured.snapshot();
    ASSERT_EQ(requests.size(), 2U);
    for (const Request& request : requests) {
        const auto if_range = request.headers.find("if-range");
        ASSERT_NE(if_range, request.headers.end());
        EXPECT_EQ(if_range->second, kLastModified);
    }
}

TEST_F(SegmentedEngineTest, WithoutAValidatorNoIfRangeAndNoCheckpointIsWritten) {
    const std::string payload = make_payload(static_cast<std::size_t>(2 * kMiB));
    CapturedRequests captured;
    HttpTestServer server([&payload, &captured](const Request& request) {
        captured.add(request);
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    DownloadOptions download = options(server, "novalidator.bin");
    const ProbeResult probe = make_probe(download.url, 2 * kMiB, "", "");
    const std::vector<SegmentTask> tasks = {{Segment{0, kMiB}, 0}, {Segment{kMiB, kMiB}, 0}};

    const SegmentedOutcome outcome =
        run_segmented(download, probe, tasks, nullptr, nullptr, SegmentedSettings{});
    ASSERT_TRUE(outcome.summary.ok()) << outcome.summary.message;
    EXPECT_EQ(read_file(download.output_path), payload);
    EXPECT_FALSE(file_exists(flowget::resume_sidecar_path(download.output_path)));

    const std::vector<Request> requests = captured.snapshot();
    ASSERT_EQ(requests.size(), 2U);
    for (const Request& request : requests) {
        EXPECT_EQ(request.headers.find("if-range"), request.headers.end());
    }
}

TEST_F(SegmentedEngineTest, CancellationPublishesACheckpoint) {
    const std::string payload = make_payload(static_cast<std::size_t>(4 * kMiB));
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        response.throttle_bytes_per_second = 512 * 1024;
        return response;
    });

    DownloadOptions download = options(server, "checkpoint.bin");
    const ProbeResult probe = make_probe(download.url, 4 * kMiB, "\"strong\"", "");
    const std::vector<SegmentTask> tasks = {{Segment{0, kMiB}, 0},
                                            {Segment{kMiB, kMiB}, 0},
                                            {Segment{2 * kMiB, kMiB}, 0},
                                            {Segment{3 * kMiB, kMiB}, 0}};

    std::atomic<bool> cancel{false};
    SegmentedSettings settings;
    settings.checkpoint_interval = std::chrono::milliseconds{20};
    const SegmentedOutcome outcome = run_segmented(
        download,
        probe,
        tasks,
        [&cancel](const DownloadProgress& update) {
            if (update.bytes_written >= 512 * 1024) {
                cancel.store(true);
            }
        },
        &cancel,
        settings);
    EXPECT_EQ(outcome.summary.http.error, HttpError::aborted);

    const std::string sidecar = flowget::resume_sidecar_path(download.output_path);
    ASSERT_TRUE(file_exists(sidecar));
    std::string contents;
    std::string error;
    ASSERT_TRUE(
        flowget::detail::read_file_utf8(sidecar, flowget::kMaxResumeStateBytes, contents, error))
        << error;
    const ResumeParseResult parsed = flowget::parse_resume_state(contents);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.state.url, download.url);
    EXPECT_EQ(parsed.state.total_bytes, 4 * kMiB);
    EXPECT_GT(parsed.state.bytes_done(), 0);
    EXPECT_TRUE(file_exists(download.output_path));
}

TEST_F(SegmentedEngineTest, FlushFailureEndsWithIoAndKeepsTheOldSidecar) {
    const std::string payload = make_payload(static_cast<std::size_t>(4 * kMiB));
    HttpTestServer server([&payload](const Request&) {
        Response response;
        response.status = 200;
        response.honor_range = true;
        response.body = payload;
        return response;
    });

    DownloadOptions download = options(server, "flushfail.bin");
    const ProbeResult probe = make_probe(download.url, 4 * kMiB, "\"strong\"", "");
    const std::vector<SegmentTask> tasks = {{Segment{0, kMiB}, 512 * 1024},
                                            {Segment{kMiB, kMiB}, 0},
                                            {Segment{2 * kMiB, kMiB}, 0},
                                            {Segment{3 * kMiB, kMiB}, 0}};

    write_prefix(download.output_path, payload, 512 * 1024);
    const std::string sidecar = flowget::resume_sidecar_path(download.output_path);
    const std::string previous = "previous sidecar\n";
    {
        std::ofstream file{from_utf8(sidecar), std::ios::binary | std::ios::trunc};
        file << previous;
    }

    SegmentedSettings settings;
    settings.storage_factory = [] {
        return std::make_unique<FlushFailingStorage>(flowget::make_platform_storage());
    };
    const SegmentedOutcome outcome =
        run_segmented(download, probe, tasks, nullptr, nullptr, settings);

    EXPECT_EQ(outcome.summary.http.error, HttpError::io);
    EXPECT_NE(outcome.summary.message.find("flush"), std::string::npos) << outcome.summary.message;
    EXPECT_EQ(read_file(sidecar), previous);
    EXPECT_FALSE(file_exists(sidecar + ".tmp"));
    EXPECT_TRUE(file_exists(download.output_path));
}

} // namespace

#include "segmented_download.h"

#include <flowget/http_range.h>
#include <flowget/storage.h>

#include "curl_common.h"
#include "download_util.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace flowget::detail {
namespace {

using namespace std::chrono_literals;

// One worker for one byte range. A unique_ptr keeps the address stable because libcurl stores the
// pointer in the easy handle's callbacks.
struct Transfer {
    Segment segment;
    IStorage* storage = nullptr;
    std::int64_t expected_total = -1;
    std::string expected_etag;

    EasyHandle easy;
    bool added = false;
    bool finished = false;
    CURLcode result = CURLE_OK;
    char error_buffer[CURL_ERROR_SIZE] = {};

    // Per-response metadata, reset by a new status line (redirect chain).
    long status = 0;
    bool headers_complete = false;
    std::int64_t content_length = -1;
    std::string content_range;
    std::string etag;

    std::int64_t written = 0;
    std::string mismatch;
    std::string io_error;
};

struct ProgressState {
    const std::atomic<bool>* cancel = nullptr;
};

long parse_status_line(std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return 0;
    }
    const std::string_view code = line.substr(space + 1, 3);
    if (code.size() != 3) {
        return 0;
    }
    long value = 0;
    for (const char character : code) {
        if (character < '0' || character > '9') {
            return 0;
        }
        value = value * 10 + (character - '0');
    }
    return value;
}

bool parse_int64(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// Checks the headers of the final response before any body byte is written. A mismatch aborts the
// transfer with CURLE_WRITE_ERROR, and the caller turns the recorded reason into a fallback.
void validate_headers(Transfer& transfer) {
    transfer.headers_complete = true;

    // A redirect hop is not the response that carries the range; libcurl follows it and starts a
    // new header block, which resets the state below.
    if (transfer.status >= 300 && transfer.status < 400) {
        return;
    }
    if (transfer.status != 206) {
        transfer.mismatch =
            "server answered " + std::to_string(transfer.status) + " instead of 206";
        return;
    }

    const std::optional<ContentRange> range = parse_content_range(transfer.content_range);
    const std::int64_t last_expected = transfer.segment.offset + transfer.segment.length - 1;
    if (!range.has_value() || range->first != transfer.segment.offset ||
        range->last != last_expected) {
        transfer.mismatch = "Content-Range mismatch";
        return;
    }
    if (range->total >= 0 && transfer.expected_total >= 0 &&
        range->total != transfer.expected_total) {
        transfer.mismatch = "Content-Range total mismatch";
        return;
    }
    // Content-Length is optional (connection-close framing), but when present it must describe
    // exactly the requested range.
    if (transfer.content_length >= 0 && transfer.content_length != transfer.segment.length) {
        transfer.mismatch = "Content-Length mismatch";
        return;
    }
    if (!transfer.expected_etag.empty() && !transfer.etag.empty() &&
        transfer.etag != transfer.expected_etag) {
        transfer.mismatch = "ETag mismatch";
    }
}

std::size_t on_header(char* buffer, std::size_t size, std::size_t count, void* userdata) {
    auto& transfer = *static_cast<Transfer*>(userdata);
    const std::size_t total = size * count;
    const std::string_view line{buffer, total};

    if (line.rfind("HTTP/", 0) == 0) {
        transfer.status = parse_status_line(line);
        transfer.headers_complete = false;
        transfer.content_length = -1;
        transfer.content_range.clear();
        transfer.etag.clear();
        return total;
    }
    if (line == "\r\n") {
        validate_headers(transfer);
        return transfer.mismatch.empty() ? total : 0;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return total;
    }
    const std::string name = lowercase(trim(line.substr(0, colon)));
    const std::string_view value = trim(line.substr(colon + 1));
    if (name == "content-range") {
        transfer.content_range = std::string{value};
    } else if (name == "content-length") {
        std::int64_t length = -1;
        if (parse_int64(value, length)) {
            transfer.content_length = length;
        }
    } else if (name == "etag") {
        transfer.etag = std::string{value};
    }
    return total;
}

std::size_t on_body(char* buffer, std::size_t size, std::size_t count, void* userdata) {
    auto& transfer = *static_cast<Transfer*>(userdata);
    const std::size_t total = size * count;
    if (!transfer.mismatch.empty() || !transfer.io_error.empty()) {
        return 0;
    }

    const std::int64_t remaining = transfer.segment.length - transfer.written;
    if (remaining < 0 || static_cast<std::int64_t>(total) > remaining) {
        transfer.mismatch = "segment returned more bytes than requested";
        return 0;
    }

    std::size_t written = 0;
    const StorageResult result =
        transfer.storage->write_at(transfer.segment.offset + transfer.written,
                                   reinterpret_cast<const std::byte*>(buffer),
                                   total,
                                   written);
    transfer.written += static_cast<std::int64_t>(written);
    if (!result.ok()) {
        transfer.io_error =
            "cannot write " + std::to_string(total) + " bytes at offset " +
            std::to_string(transfer.segment.offset + transfer.written) + ": " +
            (result.message.empty() ? storage_error_name(result.error) : result.message);
        return 0;
    }
    return total;
}

int on_xferinfo(void* userdata,
                curl_off_t /*dltotal*/,
                curl_off_t /*dlnow*/,
                curl_off_t /*ultotal*/,
                curl_off_t /*ulnow*/) {
    const auto& state = *static_cast<const ProgressState*>(userdata);
    if (state.cancel != nullptr && state.cancel->load(std::memory_order_relaxed)) {
        return 1;
    }
    return 0;
}

std::int64_t sum_written(const std::vector<std::unique_ptr<Transfer>>& transfers) {
    std::int64_t total = 0;
    for (const auto& transfer : transfers) {
        total += transfer->written;
    }
    return total;
}

std::string storage_failure(const std::string& action, const StorageResult& result) {
    return action + ": " +
           (result.message.empty() ? storage_error_name(result.error) : result.message);
}

} // namespace

SegmentedOutcome run_segmented(const DownloadOptions& options,
                               const ProbeResult& probe,
                               const std::vector<Segment>& segments,
                               const ProgressCallback& on_progress,
                               const std::atomic<bool>* cancel) {
    SegmentedOutcome outcome;
    DownloadSummary& summary = outcome.summary;
    summary.connections = static_cast<int>(segments.size());

    std::string filesystem_error;
    if (!ensure_parent_directory(options.output_path, filesystem_error)) {
        summary.http.error = HttpError::io;
        summary.message = std::move(filesystem_error);
        return outcome;
    }

    const auto storage = make_platform_storage();
    const StorageResult opened = storage->open(options.output_path, OpenMode::read_write_truncate);
    if (!opened.ok()) {
        summary.http.error = HttpError::io;
        summary.message = storage_failure("cannot open " + options.output_path, opened);
        return outcome;
    }

    const std::int64_t total = std::accumulate(
        segments.begin(), segments.end(), std::int64_t{0}, [](std::int64_t sum, const Segment& s) {
            return sum + s.length;
        });
    const StorageResult preallocated = storage->preallocate(total);
    if (storage_error_is_fatal(preallocated.error)) {
        summary.http.error = HttpError::io;
        summary.message = storage_failure("cannot reserve space", preallocated);
        return outcome;
    }

    MultiHandle multi{curl_multi_init()};
    if (!multi) {
        summary.http.error = HttpError::internal;
        summary.message = "libcurl multi handle allocation failed";
        return outcome;
    }
    // Pin the pool to the plan size. libcurl's connection cache would otherwise keep serving an
    // idle connection instead of opening the N parallel ones the plan assumes.
    const auto connection_limit = static_cast<long>(segments.size());
    static_cast<void>(
        curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, connection_limit));
    static_cast<void>(
        curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, connection_limit));

    // If-Range makes a server that changed the representation answer 200 instead of a stale 206;
    // the status check below then turns that into a fallback instead of a mixed-version file.
    std::map<std::string, std::string> request_headers = options.headers;
    if (!probe.etag.empty()) {
        request_headers["If-Range"] = probe.etag;
    }

    // One list shared by every easy handle; it must outlive all transfers.
    std::optional<HeaderList> outgoing_headers = make_header_list(request_headers);
    if (!outgoing_headers.has_value()) {
        summary.http.error = HttpError::internal;
        summary.message = "failed to build request headers";
        return outcome;
    }

    ProgressState progress_state;
    progress_state.cancel = cancel;

    std::vector<std::unique_ptr<Transfer>> transfers;
    transfers.reserve(segments.size());
    std::map<CURL*, Transfer*> by_handle;
    bool setup_failed = false;

    for (const Segment& segment : segments) {
        auto transfer = std::make_unique<Transfer>();
        transfer->segment = segment;
        transfer->storage = storage.get();
        transfer->expected_total = total;
        transfer->expected_etag = probe.etag;
        transfer->easy.reset(curl_easy_init());
        if (!transfer->easy) {
            setup_failed = true;
            break;
        }

        HttpRequest request;
        request.url = options.url;
        request.user_agent = options.user_agent;
        request.range_start = segment.offset;
        request.range_length = segment.length;
        if (!configure_easy(
                transfer->easy.get(), request, transfer->error_buffer, outgoing_headers->get())) {
            setup_failed = true;
            break;
        }
        CURL* handle = transfer->easy.get();
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, transfer.get());
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, on_xferinfo);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress_state);
        by_handle.emplace(handle, transfer.get());

        if (curl_multi_add_handle(multi.get(), handle) != CURLM_OK) {
            setup_failed = true;
            break;
        }
        transfer->added = true;
        transfers.push_back(std::move(transfer));
    }

    if (setup_failed) {
        for (auto& transfer : transfers) {
            if (transfer->added) {
                curl_multi_remove_handle(multi.get(), transfer->easy.get());
                transfer->added = false;
            }
        }
        summary.http.error = HttpError::internal;
        summary.message = "failed to start the segmented transfer";
        return outcome;
    }

    // Progress is reported from this thread at most every 100 ms; a 1-byte per-callback update
    // would make the UI the bottleneck of the download.
    auto last_report = std::chrono::steady_clock::now();
    auto report = [&](bool force) {
        if (on_progress == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_report < 100ms) {
            return;
        }
        last_report = now;
        DownloadProgress update;
        update.bytes_written = sum_written(transfers);
        update.total_bytes = total;
        on_progress(update);
    };

    std::string multi_error;
    int running = 0;
    while (true) {
        const CURLMcode multi_code = curl_multi_perform(multi.get(), &running);
        if (multi_code != CURLM_OK) {
            multi_error = curl_multi_strerror(multi_code);
            break;
        }

        int queued = 0;
        while (const CURLMsg* message = curl_multi_info_read(multi.get(), &queued)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            const auto iterator = by_handle.find(message->easy_handle);
            if (iterator == by_handle.end()) {
                continue;
            }
            Transfer& transfer = *iterator->second;
            transfer.result = message->data.result;
            transfer.finished = true;
            curl_multi_remove_handle(multi.get(), message->easy_handle);
            transfer.added = false;
        }

        report(false);

        const bool cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
        bool abort_remaining = cancelled;
        for (const auto& transfer : transfers) {
            if (!transfer->mismatch.empty() || !transfer->io_error.empty()) {
                abort_remaining = true;
            }
        }
        if (abort_remaining) {
            // Stop every other connection immediately; the outcome is decided below.
            for (auto& transfer : transfers) {
                if (transfer->added) {
                    curl_multi_remove_handle(multi.get(), transfer->easy.get());
                    transfer->added = false;
                }
            }
            break;
        }

        if (running == 0) {
            break;
        }
        // 50 ms poll interval bounds cancellation latency without busy-waiting.
        static_cast<void>(curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr));
    }

    summary.bytes_written = sum_written(transfers);

    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        summary.http.error = HttpError::aborted;
        summary.message = "download cancelled";
        return outcome;
    }
    if (!multi_error.empty()) {
        summary.http.error = HttpError::internal;
        summary.message = multi_error;
        return outcome;
    }

    // Disk failures are never retried as a single stream: the same write would fail again.
    for (const auto& transfer : transfers) {
        if (!transfer->io_error.empty()) {
            summary.http.error = HttpError::io;
            summary.message = transfer->io_error;
            return outcome;
        }
    }

    // A semantic mismatch means the server does not honour ranges reliably. Report it to the caller
    // instead of retrying the whole file over N broken connections.
    for (const auto& transfer : transfers) {
        if (!transfer->mismatch.empty()) {
            outcome.fallback_reason = transfer->mismatch;
            return outcome;
        }
    }

    for (const auto& transfer : transfers) {
        if (!transfer->finished) {
            summary.http.error = HttpError::internal;
            summary.message = "a segment did not finish";
            return outcome;
        }
        if (transfer->result != CURLE_OK) {
            summary.http.error = map_curl_error(transfer->result);
            summary.message = transfer->error_buffer[0] != '\0'
                                  ? std::string{transfer->error_buffer}
                                  : curl_easy_strerror(transfer->result);
            return outcome;
        }
        if (transfer->written != transfer->segment.length) {
            outcome.fallback_reason = "segment byte count mismatch";
            return outcome;
        }
    }

    // Synthesise the response from the probe: the individual 206s describe the same representation
    // that the caller asked for, and a 206 status is what a range-capable server answered.
    const StorageResult flushed = storage->flush();
    if (!flushed.ok()) {
        summary.http.error = HttpError::io;
        summary.message = storage_failure("cannot flush the downloaded file", flushed);
        return outcome;
    }
    static_cast<void>(storage->close());

    summary.http.error = HttpError::none;
    summary.http.response.status = 206;
    summary.http.response.content_length = total;
    summary.http.response.effective_url = probe.effective_url;
    summary.http.response.etag = probe.etag;
    summary.http.response.last_modified = probe.last_modified;
    summary.http.response.accept_ranges = probe.accept_ranges;
    summary.http.response.content_type = probe.content_type;
    summary.http.response.content_disposition = probe.content_disposition;
    if (total > 0) {
        summary.http.response.content_range =
            "bytes 0-" + std::to_string(total - 1) + "/" + std::to_string(total);
    }
    summary.bytes_written = total;
    report(true);
    return outcome;
}

} // namespace flowget::detail

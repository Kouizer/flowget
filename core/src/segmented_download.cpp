#include "segmented_download.h"

#include <flowget/http_range.h>
#include <flowget/resume_state.h>
#include <flowget/storage.h>

#include "curl_common.h"
#include "download_util.h"
#include "resume_checkpoint.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
    // The range actually requested: [range start + done, range end). `done_base` is the part that
    // was already on disk before this attempt.
    Segment segment;
    std::int64_t done_base = 0;
    std::size_t task_index = 0;
    IStorage* storage = nullptr;
    std::int64_t expected_total = -1;
    std::string expected_etag;

    EasyHandle easy;
    bool added = false;
    bool finished = false;
    CURLcode result = CURLE_OK;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};

    // This transfer asked the server to prove byte identity with If-Range. A 200 then means the
    // representation changed; that is a resume invalidation, not a generic range failure.
    bool resume_expected = false;
    bool resume_invalidated = false;

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
        value = (value * 10) + (character - '0');
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
        if (transfer.resume_expected && transfer.status == 200) {
            transfer.resume_invalidated = true;
        }
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

    if (line.starts_with("HTTP/")) {
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
    const StorageResult result = transfer.storage->write_at(
        transfer.segment.offset + transfer.written,
        // libcurl hands the body over as char*; IStorage takes bytes.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C callback boundary
        reinterpret_cast<const std::byte*>(buffer),
        total,
        written);
    if (!result.ok()) {
        transfer.io_error =
            "cannot write " + std::to_string(total) + " bytes at offset " +
            std::to_string(transfer.segment.offset + transfer.written) + ": " +
            (result.message.empty() ? storage_error_name(result.error) : result.message);
        return 0;
    }
    if (written != total) {
        transfer.io_error = "short write of " + std::to_string(written) + " of " +
                            std::to_string(total) + " bytes at offset " +
                            std::to_string(transfer.segment.offset + transfer.written);
        return 0;
    }
    // `done` may only count bytes the storage layer accepted: a failed write can leave part of the
    // buffer on disk, and claiming it in a checkpoint would skip that hole on the next run.
    transfer.written += static_cast<std::int64_t>(written);
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

// RFC 9110 §13.1.5: If-Range needs a strong validator. A weak ETag proves semantic equivalence,
// not byte identity, so a weak ETag falls back to Last-Modified; without either the engine must
// not resume (and does not checkpoint, because a later run could not verify the file).
std::string select_if_range(const ProbeResult& probe) {
    if (is_strong_etag(probe.etag)) {
        return probe.etag;
    }
    return probe.last_modified;
}

ResumeState build_checkpoint_state(const DownloadOptions& options,
                                   const ProbeResult& probe,
                                   std::int64_t total,
                                   const std::vector<SegmentTask>& tasks,
                                   const std::vector<std::unique_ptr<Transfer>>& transfers) {
    ResumeState state;
    // The sidecar identifies the user's request, not the CDN hop it resolved to: signed redirect
    // targets change between runs while the representation stays the same (tasks/todo.md S5).
    state.url = options.url;
    state.total_bytes = total;
    state.etag = probe.etag;
    state.last_modified = probe.last_modified;
    state.segments.reserve(tasks.size());
    for (const SegmentTask& task : tasks) {
        state.segments.push_back(
            ResumeSegment{task.range.offset, task.range.offset + task.range.length, task.done});
    }
    for (const auto& transfer : transfers) {
        ResumeSegment& segment = state.segments[transfer->task_index];
        segment.done = transfer->done_base + transfer->written;
    }
    return state;
}

} // namespace

SegmentedOutcome run_segmented(const DownloadOptions& options,
                               const ProbeResult& probe,
                               const std::vector<SegmentTask>& tasks,
                               const ProgressCallback& on_progress,
                               const std::atomic<bool>* cancel,
                               const SegmentedSettings& settings) {
    SegmentedOutcome outcome;
    DownloadSummary& summary = outcome.summary;
    summary.connections = static_cast<int>(tasks.size());

    // A resumed attempt must never truncate: the bytes already on disk are the whole point.
    std::int64_t total = 0;
    std::int64_t base_done = 0;
    bool resumed = false;
    for (const SegmentTask& task : tasks) {
        if (task.range.offset < 0 || task.range.length < 0 || task.done < 0 ||
            task.done > task.range.length) {
            summary.http.error = HttpError::internal;
            summary.message = "invalid segment task";
            return outcome;
        }
        total += task.range.length;
        base_done += task.done;
        resumed = resumed || task.done > 0;
    }
    if (probe.content_length >= 0 && total != probe.content_length) {
        summary.http.error = HttpError::internal;
        summary.message = "the segment plan does not cover the probed size";
        return outcome;
    }

    std::string filesystem_error;
    if (!ensure_parent_directory(options.output_path, filesystem_error)) {
        summary.http.error = HttpError::io;
        summary.message = std::move(filesystem_error);
        return outcome;
    }

    std::unique_ptr<IStorage> storage =
        settings.storage_factory != nullptr ? settings.storage_factory() : make_platform_storage();
    if (storage == nullptr) {
        summary.http.error = HttpError::internal;
        summary.message = "storage allocation failed";
        return outcome;
    }
    const OpenMode mode = resumed ? OpenMode::read_write : OpenMode::read_write_truncate;
    const StorageResult opened = storage->open(options.output_path, mode);
    if (!opened.ok()) {
        summary.http.error = HttpError::io;
        summary.message = storage_failure("cannot open " + options.output_path, opened);
        return outcome;
    }

    const StorageResult preallocated = storage->preallocate(total);
    if (storage_error_is_fatal(preallocated.error)) {
        summary.http.error = HttpError::io;
        summary.message = storage_failure("cannot reserve space", preallocated);
        return outcome;
    }

    const MultiHandle multi{curl_multi_init()};
    if (!multi) {
        summary.http.error = HttpError::internal;
        summary.message = "libcurl multi handle allocation failed";
        return outcome;
    }
    // Pin the pool to the active plan size. libcurl's connection cache would otherwise keep serving
    // an idle connection instead of opening the N parallel ones the plan assumes.
    const auto connection_limit = static_cast<long>(std::max<std::size_t>(tasks.size(), 1));
    static_cast<void>(
        curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, connection_limit));
    static_cast<void>(
        curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, connection_limit));

    // If-Range makes a server that changed the representation answer 200 instead of a stale 206;
    // the status check below then turns that into a resume invalidation or a fallback instead of a
    // mixed-version file.
    const std::string if_range = select_if_range(probe);
    std::map<std::string, std::string> request_headers = options.headers;
    if (!if_range.empty()) {
        request_headers["If-Range"] = if_range;
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
    transfers.reserve(tasks.size());
    std::map<CURL*, Transfer*> by_handle;
    bool setup_failed = false;

    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const SegmentTask& task = tasks[index];
        const std::int64_t remaining = task.range.length - task.done;
        if (remaining == 0) {
            // Already durable: no request and no connection for this range.
            continue;
        }

        auto transfer = std::make_unique<Transfer>();
        transfer->segment = Segment{task.range.offset + task.done, task.range.length - task.done};
        transfer->done_base = task.done;
        transfer->task_index = index;
        transfer->storage = storage.get();
        transfer->expected_total = total;
        transfer->expected_etag = probe.etag;
        transfer->resume_expected = !if_range.empty() && task.done > 0;
        transfer->easy.reset(curl_easy_init());
        if (!transfer->easy) {
            setup_failed = true;
            break;
        }

        HttpRequest request;
        // The probe followed the redirect chain; segment requests must target the hop that actually
        // serves the representation (signed CDN URLs differ from the user's original URL).
        request.url = probe.effective_url.empty() ? options.url : probe.effective_url;
        request.user_agent = options.user_agent;
        request.range_start = transfer->segment.offset;
        request.range_length = transfer->segment.length;
        if (!configure_easy(transfer->easy.get(),
                            request,
                            transfer->error_buffer.data(),
                            outgoing_headers->get())) {
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
    summary.connections = static_cast<int>(transfers.size());

    // Without a strong validator a later run cannot prove byte identity, so writing a sidecar would
    // only produce a file the next run must reject.
    const bool checkpoint_enabled = !if_range.empty() && !tasks.empty();
    const std::string sidecar_path =
        checkpoint_enabled ? resume_sidecar_path(options.output_path) : std::string{};
    std::int64_t checkpointed_bytes = base_done;
    std::string checkpoint_error;
    bool fatal_flush_failure = false;

    auto attempt_checkpoint = [&]() {
        if (!checkpoint_enabled) {
            return CheckpointResult::published;
        }
        const std::int64_t done_now = base_done + sum_written(transfers);
        if (done_now == checkpointed_bytes) {
            // Nothing new is durable since the last sidecar: skip the fsync.
            return CheckpointResult::published;
        }
        std::string error;
        const CheckpointResult result = publish_resume_checkpoint(
            *storage,
            sidecar_path,
            build_checkpoint_state(options, probe, total, tasks, transfers),
            error);
        if (result == CheckpointResult::published) {
            checkpointed_bytes = done_now;
            checkpoint_error.clear();
        } else {
            checkpoint_error = std::move(error);
        }
        return result;
    };

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
        update.bytes_written = base_done + sum_written(transfers);
        update.total_bytes = total;
        on_progress(update);
    };

    std::string multi_error;
    int running = 0;
    auto next_checkpoint = std::chrono::steady_clock::now() + settings.checkpoint_interval;
    while (true) {
        const CURLMcode multi_code = curl_multi_perform(multi.get(), &running);
        if (multi_code != CURLM_OK) {
            multi_error = curl_multi_strerror(multi_code);
            break;
        }

        bool segment_finished = false;
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
            // libcurl's CURLMsg::data is a union by design; `result` is the documented member after
            // CURLMSG_DONE and has no variant equivalent.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
            transfer.result = message->data.result;
            transfer.finished = true;
            segment_finished = true;
            curl_multi_remove_handle(multi.get(), message->easy_handle);
            transfer.added = false;
        }

        report(false);

        if (segment_finished && attempt_checkpoint() == CheckpointResult::data_flush_failed) {
            fatal_flush_failure = true;
            break;
        }

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

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_checkpoint) {
            next_checkpoint = now + settings.checkpoint_interval;
            if (attempt_checkpoint() == CheckpointResult::data_flush_failed) {
                fatal_flush_failure = true;
                break;
            }
        }
    }

    summary.bytes_written = base_done + sum_written(transfers);

    const bool cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
    const bool write_failed = std::any_of(
        transfers.begin(), transfers.end(), [](const auto& t) { return !t->io_error.empty(); });
    const bool any_mismatch = std::any_of(
        transfers.begin(), transfers.end(), [](const auto& t) { return !t->mismatch.empty(); });

    // Cancel and transport/disk failures keep the partial file, so publish whatever is durable
    // before returning. A data flush failure is terminal: the sidecar must not claim those bytes.
    if (!fatal_flush_failure && (cancelled || write_failed || !multi_error.empty())) {
        if (attempt_checkpoint() == CheckpointResult::data_flush_failed) {
            fatal_flush_failure = true;
        }
    }

    if (cancelled && !fatal_flush_failure) {
        summary.http.error = HttpError::aborted;
        summary.message = "download cancelled";
        return outcome;
    }
    if (fatal_flush_failure) {
        summary.http.error = HttpError::io;
        summary.message =
            checkpoint_error.empty() ? "cannot flush the downloaded data" : checkpoint_error;
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
    if (any_mismatch) {
        outcome.resume_invalidated =
            std::any_of(transfers.begin(), transfers.end(), [](const auto& t) {
                return t->resume_invalidated;
            });
        for (const auto& transfer : transfers) {
            if (!transfer->mismatch.empty()) {
                outcome.fallback_reason = transfer->mismatch;
                return outcome;
            }
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
                                  ? std::string{transfer->error_buffer.data()}
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

    if (checkpoint_enabled) {
        // The output is complete; a stale sidecar would only make the next run re-verify it. A
        // failure here is not fatal: the next run deletes it through the "already complete" path.
        std::string ignored;
        static_cast<void>(remove_file(sidecar_path, ignored));
    }

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

#include <flowget/resume_state.h>
#include <flowget/single_stream_download.h>
#include <flowget/storage.h>

#include "download_util.h"
#include "file_replace.h"
#include "resume_checkpoint.h"
#include "resume_sidecar.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace flowget {
namespace {

using namespace std::chrono_literals;
using detail::CheckpointResult;

struct AttemptResult {
    DownloadSummary summary;
    // True only when every body byte was written and the final data flush succeeded.
    bool completed = false;
    // The server answered a full request with a partial body: not a resumable condition.
    std::string bad_response;
};

ResumeState single_stream_state(const DownloadOptions& options,
                                std::int64_t total,
                                const std::string& etag,
                                const std::string& last_modified,
                                std::int64_t done) {
    ResumeState state;
    state.url = options.url;
    state.total_bytes = total;
    state.etag = etag;
    state.last_modified = last_modified;
    state.segments = {ResumeSegment{0, total, done}};
    return state;
}

// One fresh request. The file is truncated, then preallocated to the announced length, so the
// sidecar's `done` (not the file size) is the only meaningful progress until the transfer ends.
AttemptResult run_fresh_attempt(HttpClient& client,
                                const DownloadOptions& options,
                                std::unique_ptr<IStorage> storage,
                                const std::string& sidecar_path,
                                const ProgressCallback& on_progress,
                                const std::atomic<bool>* cancel,
                                std::chrono::milliseconds checkpoint_interval) {
    AttemptResult attempt;
    DownloadSummary& summary = attempt.summary;
    summary.connections = 1;

    const StorageResult opened = storage->open(options.output_path, OpenMode::read_write_truncate);
    if (!opened.ok()) {
        summary.http.error = HttpError::io;
        summary.message =
            "cannot open " + options.output_path + " for writing: " +
            (opened.message.empty() ? storage_error_name(opened.error) : opened.message);
        return attempt;
    }

    std::int64_t total = -1;
    std::int64_t offset = 0;
    std::string etag;
    std::string last_modified;
    std::string if_range;
    bool response_seen = false;
    bool write_allowed = false;
    bool checkpoint_enabled = false;
    bool write_failed = false;
    bool checkpoint_fatal = false;
    std::string write_message;
    std::string checkpoint_error;
    std::int64_t checkpointed_bytes = 0;

    // S5 checkpoint order: data flush first, then the atomically replaced sidecar. A data flush
    // failure is terminal; a sidecar write failure is retried at the next trigger.
    const auto publish = [&]() -> bool {
        if (!checkpoint_enabled || offset == checkpointed_bytes) {
            return true;
        }
        std::string error;
        const CheckpointResult result = detail::publish_resume_checkpoint(
            *storage,
            sidecar_path,
            single_stream_state(options, total, etag, last_modified, offset),
            error);
        if (result == CheckpointResult::published) {
            checkpointed_bytes = offset;
            checkpoint_error.clear();
            return true;
        }
        checkpoint_error = std::move(error);
        return result != CheckpointResult::data_flush_failed;
    };

    auto last_report = std::chrono::steady_clock::now();
    auto next_checkpoint = last_report + checkpoint_interval;

    const BodySink sink = [&](const std::byte* data, std::size_t size) {
        if (write_failed || checkpoint_fatal) {
            return false;
        }
        if (!write_allowed) {
            // An error body, or a response that must not be written: drain it and let the
            // transport report the status.
            return true;
        }
        std::size_t written = 0;
        const StorageResult result = storage->write_at(offset, data, size, written);
        if (!result.ok() || written != size) {
            write_failed = true;
            write_message =
                result.message.empty() ? storage_error_name(result.error) : result.message;
            return false;
        }
        offset += static_cast<std::int64_t>(written);

        if (on_progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= 100ms) {
                last_report = now;
                on_progress(DownloadProgress{offset, total});
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (checkpoint_enabled && now >= next_checkpoint) {
            next_checkpoint = now + checkpoint_interval;
            if (!publish()) {
                checkpoint_fatal = true;
                return false;
            }
        }
        return true;
    };

    HttpRequest request;
    request.url = options.url;
    request.user_agent = options.user_agent;
    request.headers = options.headers;

    // The transport calls this before the first body byte, which is the only point where the body
    // sink can learn the file size (preallocation) and the validator (checkpoint decision).
    request.on_response = [&](const HttpResponse& response) {
        if (response_seen) {
            return;
        }
        response_seen = true;

        if (response.status >= 200 && response.status < 300 && response.status != 206) {
            total = response.content_length;
            etag = response.etag;
            last_modified = response.last_modified;
            if (total >= 0) {
                const StorageResult preallocated = storage->preallocate(total);
                if (storage_error_is_fatal(preallocated.error)) {
                    write_failed = true;
                    write_message = preallocated.message.empty()
                                        ? storage_error_name(preallocated.error)
                                        : preallocated.message;
                    return;
                }
            }
            // S5b: without Content-Length the file size cannot be verified, so no sidecar.
            if_range = detail::select_if_range(etag, last_modified);
            checkpoint_enabled = total > 0 && !if_range.empty();
            if (options.on_resume) {
                options.on_resume(0);
            }
            write_allowed = true;
            return;
        }
        if (response.status == 206) {
            attempt.bad_response = "the server returned a partial response to a full request";
        }
        // 4xx/5xx: the body is not the resource. Drain it; the transport reports the status.
    };

    summary.http = client.get(request, sink, cancel);

    if (write_failed) {
        summary.http.error = HttpError::io;
        summary.message =
            write_message.empty() ? "failed while writing " + options.output_path : write_message;
    } else if (checkpoint_fatal) {
        summary.http.error = HttpError::io;
        summary.message =
            checkpoint_error.empty() ? "cannot flush the downloaded data" : checkpoint_error;
    }

    const bool transfer_ok =
        summary.http.ok() && write_allowed && !write_failed && !checkpoint_fatal;

    if (transfer_ok && total >= 0 && offset != total) {
        // A short body usually arrives as CURLE_PARTIAL_FILE; this covers a server that closes
        // "cleanly" before the announced length.
        summary.http.error = HttpError::connect;
        summary.message = "the transfer ended after " + std::to_string(offset) + " of " +
                          std::to_string(total) + " bytes";
    }

    if (summary.http.ok()) {
        const StorageResult flushed = storage->flush();
        if (!flushed.ok()) {
            summary.http.error = HttpError::io;
            summary.message =
                flushed.message.empty() ? storage_error_name(flushed.error) : flushed.message;
        } else {
            // Complete: a stale sidecar would only make the next run re-verify the file.
            detail::forget_sidecar(sidecar_path);
            attempt.completed = true;
        }
    } else if (checkpoint_enabled && offset > checkpointed_bytes) {
        // Cancellation and transport failures keep the durable prefix. A checkpoint failure here
        // is a disk error, not a reason to drop the file: the previous sidecar stays valid.
        if (!publish()) {
            summary.http.error = HttpError::io;
            summary.message =
                checkpoint_error.empty() ? "cannot flush the downloaded data" : checkpoint_error;
        }
    }

    const StorageResult closed = storage->close();
    if (!closed.ok() && summary.http.ok()) {
        summary.http.error = HttpError::io;
        summary.message =
            closed.message.empty() ? storage_error_name(closed.error) : closed.message;
    }

    if (attempt.completed) {
        summary.http.response.content_length = total;
    }
    summary.bytes_written = offset;
    return attempt;
}

// A failed transfer keeps the partial file only when a sidecar makes it resumable. Without one
// (no validator, or nothing written yet) the file has no meaning for the next run.
DownloadSummary finish_attempt(const DownloadOptions& options,
                               const std::string& sidecar_path,
                               const AttemptResult& attempt) {
    if (!attempt.completed && !detail::file_exists(sidecar_path)) {
        std::string remove_error;
        static_cast<void>(detail::remove_file(options.output_path, remove_error));
    }
    return attempt.summary;
}

} // namespace

SingleStreamDownloader::SingleStreamDownloader(HttpClient& client) : client_(client) {}

DownloadSummary SingleStreamDownloader::download(const DownloadOptions& options,
                                                 const ProgressCallback& on_progress,
                                                 const std::atomic<bool>* cancel,
                                                 const SingleStreamSettings& settings) {
    std::string filesystem_error;
    if (!detail::ensure_parent_directory(options.output_path, filesystem_error)) {
        DownloadSummary summary;
        summary.http.error = HttpError::io;
        summary.message = std::move(filesystem_error);
        return summary;
    }

    const std::string sidecar_path = resume_sidecar_path(options.output_path);
    // An interrupted checkpoint leaves `<output>.flowget.tmp` behind. No reader trusts that file,
    // so it is always stale.
    std::string ignored;
    static_cast<void>(detail::remove_stale_sidecar_temp(options.output_path, ignored));

    if (!options.resume) {
        // Downloader removes it strictly before this path; a direct caller gets the best effort.
        detail::forget_sidecar(sidecar_path);
    }

    std::unique_ptr<IStorage> storage =
        settings.storage_factory ? settings.storage_factory() : make_platform_storage();
    AttemptResult attempt = run_fresh_attempt(client_,
                                              options,
                                              std::move(storage),
                                              sidecar_path,
                                              on_progress,
                                              cancel,
                                              settings.checkpoint_interval);
    if (!attempt.bad_response.empty()) {
        attempt.summary.http.error = HttpError::internal;
        attempt.summary.message = attempt.bad_response;
    }
    return finish_attempt(options, sidecar_path, attempt);
}

} // namespace flowget

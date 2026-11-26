#include <flowget/http_range.h>
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
#include <optional>
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
    // The sidecar no longer describes what the server is sending. The caller deletes it and runs a
    // plain request; nothing from this attempt may reach the file.
    bool restart_fresh = false;
    // The request asked for a range, but the server answered 200 and the whole representation is
    // being rewritten from byte zero.
    bool restarted_from_zero = false;
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

// Empty when the sidecar can be resumed from; otherwise the user-facing reason to start fresh.
// Everything here is provable without a network request: the file size is the preallocated length,
// so `done` (not the file size) stays the only trusted progress value.
std::string resume_mismatch(const ResumeState& state, const DownloadOptions& options) {
    if (state.url != options.url) {
        return "the URL changed";
    }
    if (state.total_bytes <= 0) {
        return "the sidecar has no file size";
    }
    if (state.segments.size() != 1) {
        return "the sidecar is not a single-stream checkpoint";
    }
    const ResumeSegment& part = state.segments.front();
    if (part.start != 0 || part.end != state.total_bytes || part.length() != state.total_bytes) {
        return "the sidecar segment does not cover the file";
    }
    if (part.done < 0 || part.done > part.length()) {
        return "the sidecar progress is out of range";
    }
    if (detail::select_if_range(state.etag, state.last_modified).empty()) {
        return "the sidecar has no usable validator";
    }
    if (detail::file_size_or_unknown(options.output_path) != state.total_bytes) {
        return "the partial file is missing or has the wrong size";
    }
    if (part.done == 0) {
        return "the sidecar has no progress to resume from";
    }
    return {};
}

// State of one request attempt. Helpers below take it by reference and own one step each: building
// the request, validating the response headers, writing a body chunk, publishing a checkpoint and
// deciding how the attempt ended. Pointers (not references) keep the aggregate assignable.
struct AttemptContext {
    const DownloadOptions* options = nullptr;
    IStorage* storage = nullptr;
    std::string sidecar_path;
    const ProgressCallback* on_progress = nullptr;
    std::chrono::milliseconds checkpoint_interval{};
    const ResumeState* resume = nullptr;

    AttemptResult attempt;
    std::int64_t total = -1;
    std::int64_t offset = 0;
    std::string etag;
    std::string last_modified;
    std::string if_range;
    bool response_seen = false;
    bool write_allowed = false;
    // A resume already has durable bytes and a validator, so the first timer may publish at once.
    bool checkpoint_enabled = false;
    bool write_failed = false;
    bool checkpoint_fatal = false;
    std::string write_message;
    std::string checkpoint_error;
    std::int64_t checkpointed_bytes = 0;
    std::chrono::steady_clock::time_point last_report;
    std::chrono::steady_clock::time_point next_checkpoint;
};

AttemptContext make_attempt(const DownloadOptions& options,
                            IStorage& storage,
                            std::string sidecar_path,
                            const ProgressCallback& on_progress,
                            const ResumeState* resume,
                            std::chrono::milliseconds checkpoint_interval) {
    AttemptContext context;
    context.options = &options;
    context.storage = &storage;
    context.sidecar_path = std::move(sidecar_path);
    context.on_progress = &on_progress;
    context.checkpoint_interval = checkpoint_interval;
    context.resume = resume;
    context.attempt.summary.connections = 1;
    if (resume != nullptr) {
        context.total = resume->total_bytes;
        context.offset = resume->bytes_done();
        context.etag = resume->etag;
        context.last_modified = resume->last_modified;
    }
    context.if_range = detail::select_if_range(context.etag, context.last_modified);
    context.checkpoint_enabled = resume != nullptr;
    context.checkpointed_bytes = context.offset;
    return context;
}

// The file is truncated for a fresh start and left intact for a resume; in both cases it is
// preallocated to the announced length, so the sidecar's `done` (not the file size) is the only
// meaningful progress until the transfer ends.
bool open_data_file(AttemptContext& ctx) {
    const OpenMode open_mode =
        ctx.resume != nullptr ? OpenMode::read_write : OpenMode::read_write_truncate;
    const StorageResult opened = ctx.storage->open(ctx.options->output_path, open_mode);
    if (opened.ok()) {
        return true;
    }
    ctx.attempt.summary.http.error = HttpError::io;
    ctx.attempt.summary.message =
        "cannot open " + ctx.options->output_path + " for writing: " +
        (opened.message.empty() ? storage_error_name(opened.error) : opened.message);
    return false;
}

// S5 checkpoint order: data flush first, then the atomically replaced sidecar. A data flush failure
// is terminal; a sidecar write failure is retried at the next trigger.
bool publish_checkpoint(AttemptContext& ctx) {
    if (!ctx.checkpoint_enabled || ctx.offset == ctx.checkpointed_bytes) {
        return true;
    }
    std::string error;
    const CheckpointResult result = detail::publish_resume_checkpoint(
        *ctx.storage,
        ctx.sidecar_path,
        single_stream_state(*ctx.options, ctx.total, ctx.etag, ctx.last_modified, ctx.offset),
        error);
    if (result == CheckpointResult::published) {
        ctx.checkpointed_bytes = ctx.offset;
        ctx.checkpoint_error.clear();
        return true;
    }
    ctx.checkpoint_error = std::move(error);
    return result != CheckpointResult::data_flush_failed;
}

void report_progress(AttemptContext& ctx) {
    if (ctx.on_progress == nullptr || !*ctx.on_progress) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - ctx.last_report < 100ms) {
        return;
    }
    ctx.last_report = now;
    (*ctx.on_progress)(DownloadProgress{ctx.offset, ctx.total});
}

bool maybe_publish_checkpoint(AttemptContext& ctx) {
    const auto now = std::chrono::steady_clock::now();
    if (!ctx.checkpoint_enabled || now < ctx.next_checkpoint) {
        return true;
    }
    ctx.next_checkpoint = now + ctx.checkpoint_interval;
    if (publish_checkpoint(ctx)) {
        return true;
    }
    ctx.checkpoint_fatal = true;
    return false;
}

bool write_chunk(AttemptContext& ctx, const std::byte* data, std::size_t size) {
    std::size_t written = 0;
    const StorageResult result = ctx.storage->write_at(ctx.offset, data, size, written);
    if (!result.ok() || written != size) {
        ctx.write_failed = true;
        ctx.write_message =
            result.message.empty() ? storage_error_name(result.error) : result.message;
        return false;
    }
    ctx.offset += static_cast<std::int64_t>(written);
    return true;
}

bool write_body(AttemptContext& ctx, const std::byte* data, std::size_t size) {
    if (ctx.write_failed || ctx.checkpoint_fatal) {
        return false;
    }
    if (!ctx.write_allowed) {
        // An error body, or a response that must not be written: drain it and let the transport
        // report the status.
        return true;
    }
    if (!write_chunk(ctx, data, size)) {
        return false;
    }
    report_progress(ctx);
    return maybe_publish_checkpoint(ctx);
}

// A 206 to a range request must describe exactly the durable prefix the sidecar claims. If-Range
// already asked the server to fall back to 200 on a changed representation; a 206 that still
// contradicts the sidecar means the server is not trustworthy. A header the server omitted cannot
// contradict the sidecar.
bool matches_sidecar(const AttemptContext& ctx, const HttpResponse& response) {
    const std::optional<ContentRange> range = parse_content_range(response.content_range);
    const bool range_ok = range.has_value() && range->has_range() && range->first == ctx.offset &&
                          (range->last < 0 || range->last == ctx.total - 1) &&
                          (range->total < 0 || range->total == ctx.total);
    if (!range_ok) {
        return false;
    }
    if (response.content_length >= 0 && response.content_length != ctx.total - ctx.offset) {
        return false;
    }
    if (is_strong_etag(ctx.etag)) {
        return response.etag.empty() || response.etag == ctx.etag;
    }
    return response.last_modified.empty() || response.last_modified == ctx.last_modified;
}

void handle_partial_response(AttemptContext& ctx, const HttpResponse& response) {
    if (!matches_sidecar(ctx, response)) {
        ctx.attempt.restart_fresh = true;
        ctx.write_allowed = false;
        return;
    }
    if (ctx.options->on_resume) {
        ctx.options->on_resume(ctx.offset);
    }
    ctx.write_allowed = true;
}

bool restart_at_zero(AttemptContext& ctx) {
    const StorageResult closed = ctx.storage->close();
    const StorageResult reopened =
        ctx.storage->open(ctx.options->output_path, OpenMode::read_write_truncate);
    if (!closed.ok() || !reopened.ok()) {
        ctx.write_failed = true;
        ctx.write_message = !reopened.ok() && !reopened.message.empty()
                                ? reopened.message
                                : "cannot restart " + ctx.options->output_path;
        return false;
    }
    detail::forget_sidecar(ctx.sidecar_path);
    ctx.offset = 0;
    ctx.checkpointed_bytes = 0;
    ctx.attempt.restarted_from_zero = true;
    return true;
}

bool adopt_full_response(AttemptContext& ctx, const HttpResponse& response) {
    ctx.total = response.content_length;
    ctx.etag = response.etag;
    ctx.last_modified = response.last_modified;
    if (ctx.total >= 0) {
        const StorageResult preallocated = ctx.storage->preallocate(ctx.total);
        if (storage_error_is_fatal(preallocated.error)) {
            ctx.write_failed = true;
            ctx.write_message = preallocated.message.empty()
                                    ? storage_error_name(preallocated.error)
                                    : preallocated.message;
            return false;
        }
    }
    // S5b: without Content-Length the file size cannot be verified, so no sidecar.
    ctx.if_range = detail::select_if_range(ctx.etag, ctx.last_modified);
    ctx.checkpoint_enabled = ctx.total > 0 && !ctx.if_range.empty();
    if (ctx.options->on_resume) {
        ctx.options->on_resume(0);
    }
    ctx.write_allowed = true;
    return true;
}

void handle_full_response(AttemptContext& ctx, const HttpResponse& response) {
    if (ctx.resume != nullptr) {
        // 200 to a range request: the representation changed or the server ignored Range. The body
        // is the whole resource, so truncate in place instead of paying for a second request. The
        // old sidecar must not survive this decision: it points at a prefix being rewritten.
        if (!restart_at_zero(ctx)) {
            return;
        }
    }
    static_cast<void>(adopt_full_response(ctx, response));
}

void handle_response(AttemptContext& ctx, const HttpResponse& response) {
    if (ctx.response_seen) {
        return;
    }
    ctx.response_seen = true;

    if (ctx.resume != nullptr && response.status == 206) {
        handle_partial_response(ctx, response);
        return;
    }
    if (response.status >= 200 && response.status < 300 && response.status != 206) {
        handle_full_response(ctx, response);
        return;
    }
    if (response.status == 206) {
        ctx.attempt.bad_response = "the server returned a partial response to a full request";
    }
    // 4xx/5xx: the body is not the resource. Drain it; the transport reports the status.
}

HttpRequest build_request(const DownloadOptions& options,
                          const ResumeState* resume,
                          const std::string& if_range,
                          std::int64_t resumed_from) {
    HttpRequest request;
    request.url = options.url;
    request.user_agent = options.user_agent;
    request.headers = options.headers;
    if (resume != nullptr) {
        // Open-ended suffix: the server sends `total - done` bytes starting at `done`.
        request.range_start = resumed_from;
        request.range_length = -1;
        request.headers["If-Range"] = if_range;
    }
    return request;
}

void set_attempt_error(AttemptContext& ctx) {
    if (ctx.write_failed) {
        ctx.attempt.summary.http.error = HttpError::io;
        ctx.attempt.summary.message = ctx.write_message.empty()
                                          ? "failed while writing " + ctx.options->output_path
                                          : ctx.write_message;
        return;
    }
    if (ctx.checkpoint_fatal) {
        ctx.attempt.summary.http.error = HttpError::io;
        ctx.attempt.summary.message = ctx.checkpoint_error.empty()
                                          ? "cannot flush the downloaded data"
                                          : ctx.checkpoint_error;
    }
}

void complete_transfer(AttemptContext& ctx) {
    const StorageResult flushed = ctx.storage->flush();
    if (!flushed.ok()) {
        ctx.attempt.summary.http.error = HttpError::io;
        ctx.attempt.summary.message =
            flushed.message.empty() ? storage_error_name(flushed.error) : flushed.message;
        return;
    }
    // Complete: a stale sidecar would only make the next run re-verify the file.
    detail::forget_sidecar(ctx.sidecar_path);
    ctx.attempt.completed = true;
}

void close_storage(AttemptContext& ctx) {
    const StorageResult closed = ctx.storage->close();
    if (!closed.ok() && ctx.attempt.summary.http.ok()) {
        ctx.attempt.summary.http.error = HttpError::io;
        ctx.attempt.summary.message =
            closed.message.empty() ? storage_error_name(closed.error) : closed.message;
    }
}

void finish_transfer(AttemptContext& ctx) {
    DownloadSummary& summary = ctx.attempt.summary;
    set_attempt_error(ctx);

    const bool transfer_ok =
        summary.http.ok() && ctx.write_allowed && !ctx.write_failed && !ctx.checkpoint_fatal;

    if (transfer_ok && ctx.total >= 0 && ctx.offset != ctx.total) {
        // A short body usually arrives as CURLE_PARTIAL_FILE; this covers a server that closes
        // "cleanly" before the announced length.
        summary.http.error = HttpError::connect;
        summary.message = "the transfer ended after " + std::to_string(ctx.offset) + " of " +
                          std::to_string(ctx.total) + " bytes";
    }

    if (summary.http.ok()) {
        complete_transfer(ctx);
    } else if (ctx.checkpoint_enabled && ctx.offset > ctx.checkpointed_bytes) {
        // Cancellation and transport failures keep the durable prefix. A checkpoint failure here
        // is a disk error, not a reason to drop the file: the previous sidecar stays valid.
        if (!publish_checkpoint(ctx)) {
            summary.http.error = HttpError::io;
            summary.message = ctx.checkpoint_error.empty() ? "cannot flush the downloaded data"
                                                           : ctx.checkpoint_error;
        }
    }

    close_storage(ctx);

    if (ctx.attempt.completed) {
        summary.http.response.content_length = ctx.total;
    }
    summary.bytes_written = ctx.offset;
}

// One request. The caller owns the storage and the retry decision; this function only runs the
// attempt described by `ctx` and reports how it ended.
AttemptResult run_attempt(HttpClient& client,
                          const DownloadOptions& options,
                          IStorage& storage,
                          const std::string& sidecar_path,
                          const ProgressCallback& on_progress,
                          const std::atomic<bool>* cancel,
                          std::chrono::milliseconds checkpoint_interval,
                          const ResumeState* resume) {
    AttemptContext ctx =
        make_attempt(options, storage, sidecar_path, on_progress, resume, checkpoint_interval);
    if (!open_data_file(ctx)) {
        return std::move(ctx.attempt);
    }

    ctx.last_report = std::chrono::steady_clock::now();
    ctx.next_checkpoint = ctx.last_report + checkpoint_interval;

    HttpRequest request = build_request(options, resume, ctx.if_range, ctx.offset);
    // The transport calls this before the first body byte, which is the only point where the body
    // sink can learn the file size (preallocation) and the validator (checkpoint decision).
    request.on_response = [&ctx](const HttpResponse& response) { handle_response(ctx, response); };
    const BodySink sink = [&ctx](const std::byte* data, std::size_t size) {
        return write_body(ctx, data, size);
    };

    ctx.attempt.summary.http = client.get(request, sink, cancel);

    if (ctx.attempt.restart_fresh) {
        // Not an error: the caller drops the sidecar and repeats the request without a range.
        return std::move(ctx.attempt);
    }
    finish_transfer(ctx);
    return std::move(ctx.attempt);
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

DownloadSummary completed_summary(const DownloadOptions& options,
                                  const ResumeState& state,
                                  const std::string& resume_note) {
    DownloadSummary summary;
    summary.http.response.status = 206;
    summary.http.response.content_length = state.total_bytes;
    summary.http.response.effective_url = options.url;
    summary.http.response.etag = state.etag;
    summary.http.response.last_modified = state.last_modified;
    summary.bytes_written = state.total_bytes;
    summary.connections = 0;
    summary.resumed = true;
    summary.resumed_from_bytes = state.total_bytes;
    summary.resume_note = resume_note.empty() ? "already complete" : resume_note;
    return summary;
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

    ResumeState resume_state;
    bool have_resume = false;
    std::string resume_note;
    if (options.resume) {
        detail::LoadedSidecar loaded = detail::load_sidecar(sidecar_path);
        if (!loaded.discard_reason.empty()) {
            resume_note = "ignored the resume sidecar: " + loaded.discard_reason;
            detail::forget_sidecar(sidecar_path);
        } else if (loaded.exists) {
            const std::string mismatch = resume_mismatch(loaded.state, options);
            if (mismatch.empty()) {
                resume_state = std::move(loaded.state);
                have_resume = true;
            } else {
                resume_note = "restarted from zero: " + mismatch;
                detail::forget_sidecar(sidecar_path);
            }
        }
    }

    const auto make_storage = [&settings]() -> std::unique_ptr<IStorage> {
        return settings.storage_factory ? settings.storage_factory() : make_platform_storage();
    };

    if (have_resume && resume_state.bytes_done() == resume_state.total_bytes) {
        // A checkpoint only reaches the full size after every byte was flushed; no network needed.
        detail::forget_sidecar(sidecar_path);
        if (options.on_resume) {
            options.on_resume(resume_state.total_bytes);
        }
        return completed_summary(options, resume_state, resume_note);
    }

    const std::int64_t resumed_from = have_resume ? resume_state.bytes_done() : 0;
    std::unique_ptr<IStorage> storage = make_storage();
    AttemptResult attempt = run_attempt(client_,
                                        options,
                                        *storage,
                                        sidecar_path,
                                        on_progress,
                                        cancel,
                                        settings.checkpoint_interval,
                                        have_resume ? &resume_state : nullptr);
    if (attempt.restart_fresh) {
        // The sidecar pointed at a range the server no longer serves. Drop it and repeat once
        // without a range, exactly like a server that answers 200 to If-Range.
        static_cast<void>(storage->close());
        detail::forget_sidecar(sidecar_path);
        resume_note = "restarted from zero: the server no longer matches the sidecar";
        have_resume = false;
        attempt = run_attempt(client_,
                              options,
                              *storage,
                              sidecar_path,
                              on_progress,
                              cancel,
                              settings.checkpoint_interval,
                              nullptr);
    }

    if (!attempt.bad_response.empty()) {
        attempt.summary.http.error = HttpError::internal;
        attempt.summary.message = attempt.bad_response;
    }

    DownloadSummary summary = finish_attempt(options, sidecar_path, attempt);
    if (have_resume && !attempt.restarted_from_zero) {
        summary.resumed = true;
        summary.resumed_from_bytes = resumed_from;
    } else if (attempt.restarted_from_zero) {
        summary.resumed = false;
        summary.resumed_from_bytes = 0;
        resume_note = "restarted from zero: the server answered 200 to the range request";
    }
    summary.resume_note = resume_note;
    return summary;
}

} // namespace flowget

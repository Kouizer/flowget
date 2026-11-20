#include <flowget/downloader.h>
#include <flowget/probe.h>
#include <flowget/resume_state.h>
#include <flowget/segment_planner.h>
#include <flowget/single_stream_download.h>
#include <flowget/storage.h>

#include "download_util.h"
#include "file_replace.h"
#include "segmented_download.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace flowget {
namespace {

using detail::SegmentTask;

DownloadSummary failure(HttpError error, std::string message, int connections) {
    DownloadSummary summary;
    summary.http.error = error;
    summary.message = std::move(message);
    summary.connections = connections;
    return summary;
}

DownloadSummary aborted() {
    return failure(HttpError::aborted, "download cancelled", 0);
}

void apply_probe_metadata(const ProbeResult& probe, DownloadSummary& summary) {
    summary.http.response.status = probe.status;
    summary.http.response.effective_url = probe.effective_url;
    summary.http.response.etag = probe.etag;
    summary.http.response.last_modified = probe.last_modified;
    summary.http.response.accept_ranges = probe.accept_ranges;
    summary.http.response.content_type = probe.content_type;
    summary.http.response.content_disposition = probe.content_disposition;
}

// An empty representation has no bytes to fetch: create/truncate the file, do not issue a request
// that would only return another 416/200 with an empty body.
DownloadSummary create_empty_file(const DownloadOptions& options, const ProbeResult& probe) {
    std::string filesystem_error;
    if (!detail::ensure_parent_directory(options.output_path, filesystem_error)) {
        return failure(HttpError::io, std::move(filesystem_error), 0);
    }
    const auto storage = make_platform_storage();
    const StorageResult opened = storage->open(options.output_path, OpenMode::read_write_truncate);
    if (!opened.ok()) {
        return failure(
            HttpError::io,
            "cannot create " + options.output_path + ": " +
                (opened.message.empty() ? storage_error_name(opened.error) : opened.message),
            0);
    }
    DownloadSummary summary;
    apply_probe_metadata(probe, summary);
    summary.http.response.content_length = 0;
    summary.connections = 0;
    return summary;
}

// -1 means "missing or unreadable", which the resume decision treats the same way: start fresh.
std::int64_t file_size_or_unknown(const std::string& utf8_path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(detail::path_from_utf8(utf8_path), error);
    if (error) {
        return -1;
    }
    return static_cast<std::int64_t>(size);
}

bool file_exists(const std::string& utf8_path) {
    std::error_code error;
    const bool present = std::filesystem::exists(detail::path_from_utf8(utf8_path), error);
    return !error && present;
}

// Best effort: a sidecar that cannot be deleted now is caught by the next run's "already complete"
// or size check. A missing sidecar is success.
void forget_sidecar(const std::string& sidecar_path) {
    std::string ignored;
    static_cast<void>(detail::remove_file(sidecar_path, ignored));
}

struct LoadedSidecar {
    bool exists = false;
    ResumeState state;
    // Non-empty: the file exists but must not be trusted. The caller deletes it and starts fresh.
    std::string discard_reason;
};

LoadedSidecar load_sidecar(const std::string& sidecar_path) {
    LoadedSidecar loaded;
    if (!file_exists(sidecar_path)) {
        return loaded;
    }
    loaded.exists = true;

    std::string contents;
    std::string read_error;
    if (!detail::read_file_utf8(sidecar_path, kMaxResumeStateBytes, contents, read_error)) {
        loaded.discard_reason = std::move(read_error);
        return loaded;
    }
    ResumeParseResult parsed = parse_resume_state(contents);
    if (!parsed.ok()) {
        loaded.discard_reason = std::move(parsed.error);
        return loaded;
    }
    loaded.state = std::move(parsed.state);
    return loaded;
}

struct ResumePlan {
    ResumeState state;
    bool candidate = false;
    // Explains a sidecar that was read but rejected; empty when there was nothing to read.
    std::string note;
};

ResumePlan load_resume_plan(const DownloadOptions& options, const std::string& sidecar_path) {
    ResumePlan plan;
    if (!options.resume) {
        return plan;
    }
    LoadedSidecar loaded = load_sidecar(sidecar_path);
    if (!loaded.discard_reason.empty()) {
        plan.note = "ignored the resume sidecar: " + loaded.discard_reason;
        forget_sidecar(sidecar_path);
        return plan;
    }
    if (loaded.exists) {
        plan.state = std::move(loaded.state);
        plan.candidate = true;
    }
    return plan;
}

bool all_segments_done(const ResumeState& state) {
    return std::all_of(state.segments.begin(), state.segments.end(), [](const ResumeSegment& part) {
        return part.done == part.length();
    });
}

// The engine sends If-Range with a strong ETag when the server has one, otherwise Last-Modified
// (tasks/todo.md S5 decision 5). The sidecar must prove the same representation the new probe sees.
bool validators_match(const ResumeState& state, const ProbeResult& probe) {
    if (is_strong_etag(probe.etag)) {
        return state.etag == probe.etag;
    }
    return !probe.last_modified.empty() && state.last_modified == probe.last_modified;
}

// Empty when the sidecar may be resumed from; otherwise the user-facing reason to start fresh.
std::string resume_mismatch(const ResumeState& state,
                            const ProbeResult& probe,
                            const DownloadOptions& options) {
    // The sidecar records the user's original URL, not the effective one: signed CDN targets change
    // between runs while the representation stays identical (tasks/todo.md S5 decision 6).
    if (state.url != options.url) {
        return "the URL changed";
    }
    if (state.total_bytes != probe.content_length) {
        return "the size changed";
    }
    if (!validators_match(state, probe)) {
        return "the server's validator changed";
    }
    if (state.segments.size() > static_cast<std::size_t>(kMaxConnections)) {
        return "the sidecar has more segments than connections are allowed";
    }
    if (file_size_or_unknown(options.output_path) != state.total_bytes) {
        return "the partial file is missing or has the wrong size";
    }
    if (state.bytes_done() <= 0) {
        return "the sidecar has no progress to resume from";
    }
    return {};
}

std::vector<SegmentTask> resume_tasks(const ResumeState& state) {
    std::vector<SegmentTask> tasks;
    tasks.reserve(state.segments.size());
    for (const ResumeSegment& part : state.segments) {
        tasks.push_back(SegmentTask{Segment{part.start, part.length()}, part.done});
    }
    return tasks;
}

std::vector<SegmentTask> fresh_tasks(std::int64_t total, int connections) {
    const std::vector<Segment> segments = plan_segments(total, connections);
    std::vector<SegmentTask> tasks;
    tasks.reserve(segments.size());
    for (const Segment& segment : segments) {
        tasks.push_back(SegmentTask{segment, 0});
    }
    return tasks;
}

// No network request is made for an already-complete output, so the probe metadata is taken from
// the sidecar and the effective URL is the requested one.
DownloadSummary completed_summary(const DownloadOptions& options, const ResumeState& state) {
    DownloadSummary summary;
    summary.http.response.status = 206;
    summary.http.response.content_length = state.total_bytes;
    summary.http.response.effective_url = options.url;
    summary.http.response.etag = state.etag;
    summary.http.response.last_modified = state.last_modified;
    summary.bytes_written = state.total_bytes;
    summary.connections = 0;
    return summary;
}

// Empty when the probe still allows the segmented engine; otherwise why a single stream is needed.
std::string probe_fallback_reason(const ProbeResult& probe) {
    if (probe.content_length < 0) {
        return "Content-Length is unknown";
    }
    if (probe.range_support == RangeSupport::ignored) {
        return "server ignored Range";
    }
    if (probe.range_support != RangeSupport::supported) {
        return std::string{"server cannot serve ranges: "} +
               range_support_name(probe.range_support);
    }
    // Below one segment the range machinery buys nothing and risks a server that only mishandles
    // small ranges. The proven single-stream path is both simpler and faster here.
    if (probe.content_length < kMinSegmentSize) {
        return "file is smaller than the minimum segment size";
    }
    return {};
}

struct AttemptOutcome {
    DownloadSummary summary;
    // The server changed the representation while a resume was in flight; the caller may delete
    // the stale file and try once more.
    bool invalidated = false;
};

// One probe + plan + run. `state` is non-null only when the caller has a validated sidecar; a
// mismatch against this probe deletes the sidecar and continues with a fresh plan.
AttemptOutcome attempt_download(const DownloadOptions& options,
                                HttpClient& client,
                                int connections,
                                const ResumeState* state,
                                const std::string& sidecar_path,
                                const ProgressCallback& on_progress,
                                const std::atomic<bool>* cancel,
                                const std::function<void(std::int64_t)>& notify_resume,
                                std::string& resume_note) {
    AttemptOutcome result;

    ProbeOptions probe_options;
    probe_options.url = options.url;
    probe_options.user_agent = options.user_agent;
    probe_options.headers = options.headers;
    const ProbeResult probe_result = probe(client, probe_options, cancel);

    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        result.summary = aborted();
        return result;
    }
    if (!probe_result.ok()) {
        result.summary = failure(probe_result.error,
                                 probe_result.message.empty() ? http_error_name(probe_result.error)
                                                              : probe_result.message,
                                 connections);
        return result;
    }
    if (probe_result.content_length == 0) {
        forget_sidecar(sidecar_path);
        notify_resume(0);
        result.summary = create_empty_file(options, probe_result);
        return result;
    }

    bool resuming = false;
    std::int64_t resumed_from = 0;
    if (state != nullptr) {
        const std::string mismatch = resume_mismatch(*state, probe_result, options);
        if (mismatch.empty()) {
            resuming = true;
            resumed_from = state->bytes_done();
        } else {
            resume_note = "restarted from zero: " + mismatch;
            forget_sidecar(sidecar_path);
        }
    }

    const auto fallback = [&](const std::string& reason) {
        std::string removal_error;
        if (!detail::remove_file(options.output_path, removal_error)) {
            return failure(HttpError::io,
                           "cannot remove the partial file before falling back: " + removal_error,
                           connections);
        }
        forget_sidecar(sidecar_path);
        notify_resume(0);
        DownloadSummary summary =
            SingleStreamDownloader(client).download(options, on_progress, cancel);
        summary.fallback = true;
        summary.fallback_reason = reason;
        summary.connections = 1;
        summary.resume_note = resuming ? "the resume was abandoned: " + reason : resume_note;
        return summary;
    };

    const std::string fallback_reason = probe_fallback_reason(probe_result);
    if (!fallback_reason.empty()) {
        result.summary = fallback(fallback_reason);
        return result;
    }

    const std::vector<SegmentTask> tasks =
        resuming ? resume_tasks(*state) : fresh_tasks(probe_result.content_length, connections);

    notify_resume(resuming ? resumed_from : 0);
    const detail::SegmentedOutcome outcome =
        detail::run_segmented(options, probe_result, tasks, on_progress, cancel);

    if (outcome.resume_invalidated) {
        result.invalidated = true;
        return result;
    }
    if (!outcome.fallback_reason.empty()) {
        result.summary = fallback(outcome.fallback_reason);
        return result;
    }

    result.summary = outcome.summary;
    result.summary.resumed = resuming;
    result.summary.resumed_from_bytes = resuming ? resumed_from : 0;
    result.summary.resume_note = resume_note;
    return result;
}

} // namespace

Downloader::Downloader(HttpClient& client) : client_(client) {}

DownloadSummary Downloader::download(const DownloadOptions& options,
                                     const ProgressCallback& on_progress,
                                     const std::atomic<bool>* cancel) {
    const int connections = std::clamp(options.connections, 1, kMaxConnections);
    const std::string sidecar_path = resume_sidecar_path(options.output_path);

    // An interrupted checkpoint leaves `<output>.flowget.tmp` behind. No reader trusts that file,
    // so it is always stale.
    std::string ignored;
    static_cast<void>(detail::remove_stale_sidecar_temp(options.output_path, ignored));

    if (!options.resume) {
        // Not best effort: a stale sidecar could claim bytes the truncated file does not have.
        std::string removal_error;
        if (!detail::remove_file(sidecar_path, removal_error)) {
            return failure(
                HttpError::io, "cannot remove the resume sidecar: " + removal_error, connections);
        }
    }

    if (connections == 1) {
        // Single-stream resume lands in S5b. This path truncates the output, so any sidecar is
        // stale; deleting it best effort is enough because the file itself is rewritten.
        forget_sidecar(sidecar_path);
        return SingleStreamDownloader(client_).download(options, on_progress, cancel);
    }

    // One notification per download, before the first data request. The already-complete path
    // reports the full size; every other path reports 0 unless it really resumes.
    bool notified = false;
    const std::function<void(std::int64_t)> notify_resume = [&options,
                                                             &notified](std::int64_t bytes) {
        if (notified) {
            return;
        }
        notified = true;
        if (options.on_resume) {
            options.on_resume(bytes);
        }
    };

    ResumePlan plan = load_resume_plan(options, sidecar_path);
    std::string resume_note = std::move(plan.note);

    // A complete sidecar is verified against the file before any request: an output of the right
    // size needs no network at all.
    if (plan.candidate && all_segments_done(plan.state)) {
        if (file_size_or_unknown(options.output_path) == plan.state.total_bytes) {
            forget_sidecar(sidecar_path);
            notify_resume(plan.state.total_bytes);
            DownloadSummary summary = completed_summary(options, plan.state);
            summary.resumed = true;
            summary.resumed_from_bytes = plan.state.total_bytes;
            summary.resume_note = resume_note.empty() ? "already complete" : resume_note;
            return summary;
        }
        resume_note = "the sidecar was complete but the file was not; restarting";
        forget_sidecar(sidecar_path);
        plan.candidate = false;
    }

    // At most two attempts: a resume invalidated by the server (200 to If-Range) earns one fresh
    // probe and plan, never a loop.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const ResumeState* attempt_state = attempt == 0 && plan.candidate ? &plan.state : nullptr;
        const AttemptOutcome outcome = attempt_download(options,
                                                        client_,
                                                        connections,
                                                        attempt_state,
                                                        sidecar_path,
                                                        on_progress,
                                                        cancel,
                                                        notify_resume,
                                                        resume_note);
        if (!outcome.invalidated) {
            return outcome.summary;
        }

        // The representation changed: the partial file and its sidecar describe the old one.
        // Delete both, then probe and plan once more.
        std::string removal_error;
        if (!detail::remove_file(options.output_path, removal_error)) {
            return failure(HttpError::io,
                           "cannot remove the stale partial file: " + removal_error,
                           connections);
        }
        forget_sidecar(sidecar_path);
        resume_note = "the server changed while resuming; restarting from zero";
    }

    return failure(HttpError::internal, "the resume attempt did not settle", connections);
}

} // namespace flowget

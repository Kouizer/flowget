#include <flowget/downloader.h>
#include <flowget/probe.h>
#include <flowget/segment_planner.h>
#include <flowget/single_stream_download.h>
#include <flowget/storage.h>

#include "download_util.h"
#include "segmented_download.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace flowget {
namespace {

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

} // namespace

Downloader::Downloader(HttpClient& client) : client_(client) {}

DownloadSummary Downloader::download(const DownloadOptions& options,
                                     const ProgressCallback& on_progress,
                                     const std::atomic<bool>* cancel) {
    const int connections = std::clamp(options.connections, 1, kMaxConnections);
    if (connections == 1) {
        return SingleStreamDownloader(client_).download(options, on_progress, cancel);
    }

    ProbeOptions probe_options;
    probe_options.url = options.url;
    probe_options.user_agent = options.user_agent;
    probe_options.headers = options.headers;
    const ProbeResult probe_result = probe(client_, probe_options, cancel);

    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        return aborted();
    }
    if (!probe_result.ok()) {
        return failure(probe_result.error,
                       probe_result.message.empty() ? http_error_name(probe_result.error)
                                                    : probe_result.message,
                       connections);
    }
    if (probe_result.content_length == 0) {
        return create_empty_file(options, probe_result);
    }

    const auto fallback = [&](const std::string& reason) {
        DownloadSummary summary =
            SingleStreamDownloader(client_).download(options, on_progress, cancel);
        summary.fallback = true;
        summary.fallback_reason = reason;
        summary.connections = 1;
        return summary;
    };

    if (probe_result.content_length < 0) {
        return fallback("Content-Length is unknown");
    }
    if (probe_result.range_support == RangeSupport::ignored) {
        return fallback("server ignored Range");
    }
    if (probe_result.range_support != RangeSupport::supported) {
        return fallback(std::string{"server cannot serve ranges: "} +
                        range_support_name(probe_result.range_support));
    }
    // Below one segment the range machinery buys nothing and risks a server that only mishandles
    // small ranges. The proven single-stream path is both simpler and faster here.
    if (probe_result.content_length < kMinSegmentSize) {
        return fallback("file is smaller than the minimum segment size");
    }

    const std::vector<Segment> segments = plan_segments(probe_result.content_length, connections);
    detail::SegmentedOutcome outcome =
        detail::run_segmented(options, probe_result, segments, on_progress, cancel);

    if (!outcome.fallback_reason.empty()) {
        std::string removal_error;
        if (!detail::remove_file(options.output_path, removal_error)) {
            return failure(HttpError::io,
                           "cannot remove the partial file before falling back: " + removal_error,
                           connections);
        }
        return fallback(outcome.fallback_reason);
    }

    if (!outcome.summary.ok()) {
        std::string removal_error;
        if (!detail::remove_file(options.output_path, removal_error)) {
            outcome.summary.message += "; cannot remove the partial file: " + removal_error;
        }
    }
    return outcome.summary;
}

} // namespace flowget

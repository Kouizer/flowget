#ifndef FLOWGET_SEGMENTED_DOWNLOAD_H
#define FLOWGET_SEGMENTED_DOWNLOAD_H

#include <flowget/download.h>
#include <flowget/probe.h>
#include <flowget/segment_planner.h>

#include <atomic>
#include <string>
#include <vector>

namespace flowget::detail {

// Outcome of one multi-handle attempt over a planned set of ranges.
struct SegmentedOutcome {
    DownloadSummary summary;
    // Non-empty means the server cannot be trusted with ranges (it ignored Range, lied about the
    // served range, changed the ETag, or delivered the wrong number of bytes). The caller deletes
    // the partial file and retries once as a single stream. Transport and disk failures leave this
    // empty: they are hard errors and S6 retries them instead of silently restarting.
    std::string fallback_reason;
};

// Downloads `segments` (a plan for `probe.content_length` bytes) into `options.output_path`.
// All transfers run on the calling thread through libcurl multi; the file is truncated up front.
SegmentedOutcome run_segmented(const DownloadOptions& options,
                               const ProbeResult& probe,
                               const std::vector<Segment>& segments,
                               const ProgressCallback& on_progress,
                               const std::atomic<bool>* cancel);

} // namespace flowget::detail

#endif /* FLOWGET_SEGMENTED_DOWNLOAD_H */

#ifndef FLOWGET_SEGMENTED_DOWNLOAD_H
#define FLOWGET_SEGMENTED_DOWNLOAD_H

#include <flowget/download.h>
#include <flowget/probe.h>
#include <flowget/segment_planner.h>
#include <flowget/storage.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flowget::detail {

// One planned range plus the bytes that are already durable on disk. `done == range.length` means
// the range needs no request at all; otherwise the engine asks for [start + done, end).
struct SegmentTask {
    Segment range;
    std::int64_t done = 0;
};

// Outcome of one multi-handle attempt over a planned set of ranges.
struct SegmentedOutcome {
    DownloadSummary summary;
    // Non-empty means the server cannot be trusted with ranges (it ignored Range, lied about the
    // served range, changed the ETag, or delivered the wrong number of bytes). The caller deletes
    // the partial file and retries once as a single stream. Transport and disk failures leave this
    // empty: they are hard errors and S6 retries them instead of silently restarting.
    std::string fallback_reason;
    // The server answered 200 to an If-Range request: the representation changed and the partial
    // file belongs to the old one. The caller deletes file + sidecar and replans once.
    bool resume_invalidated = false;
};

// Test seam: production always uses make_platform_storage(). Tests wrap the real storage to inject
// flush/write failures without touching a second code path.
using StorageFactory = std::function<std::unique_ptr<IStorage>()>;

struct SegmentedSettings {
    StorageFactory storage_factory;
    // How often a resumed/fresh segmented download publishes a checkpoint. The default is part of
    // the S5 contract (tasks/todo.md decision 9); tests shorten it.
    std::chrono::milliseconds checkpoint_interval{2000};
};

// Downloads `tasks` into `options.output_path`. All transfers run on the calling thread through
// libcurl multi. When the probe carries a resume validator the engine publishes a sidecar
// checkpoint (flush data, then atomically replace `<output>.flowget`); without a validator there is
// nothing a later run could safely resume from, so no checkpoint is written.
//
// The file is truncated only when no task carries progress. A resumed attempt keeps the bytes
// already on disk and requests just the missing suffix of each range.
SegmentedOutcome run_segmented(const DownloadOptions& options,
                               const ProbeResult& probe,
                               const std::vector<SegmentTask>& tasks,
                               const ProgressCallback& on_progress,
                               const std::atomic<bool>* cancel,
                               const SegmentedSettings& settings = {});

} // namespace flowget::detail

#endif /* FLOWGET_SEGMENTED_DOWNLOAD_H */

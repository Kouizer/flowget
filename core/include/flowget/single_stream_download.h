#ifndef FLOWGET_SINGLE_STREAM_DOWNLOAD_H
#define FLOWGET_SINGLE_STREAM_DOWNLOAD_H

#include <flowget/download.h>
#include <flowget/http_client.h>
#include <flowget/storage.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace flowget {

// Test seam, same idea as SegmentedSettings: production uses make_platform_storage() and the 2 s
// S5 checkpoint interval; tests inject storage failures and shorten the interval.
struct SingleStreamSettings {
    std::function<std::unique_ptr<IStorage>()> storage_factory;
    std::chrono::milliseconds checkpoint_interval{2000};
};

// M1/S1b: one connection, one file. It is the fallback for servers without range support (S4) and
// the `--connections 1` path, and since S5b it resumes from the same `.flowget` sidecar format the
// segmented engine writes (one segment `[0, total)` plus the durable byte count).
class SingleStreamDownloader {
public:
    explicit SingleStreamDownloader(HttpClient& client);

    // Writes the response body to `options.output_path`. A validated single-stream sidecar makes
    // the request continue at the sidecar's durable `done` with `Range: bytes=done-` and
    // `If-Range`; a 200 answer (the server ignored the range or the representation changed) starts
    // over and truncates the file. Resume needs a known Content-Length: without one the file size
    // cannot be verified, so the transfer starts from zero and no checkpoint is written.
    //
    // Cancellation and transport failures keep the partial file and its sidecar when a checkpoint
    // makes the prefix resumable; without one (no validator, or nothing written yet) the file is
    // removed because the next run could not verify it.
    DownloadSummary download(const DownloadOptions& options,
                             const ProgressCallback& on_progress = {},
                             const std::atomic<bool>* cancel = nullptr,
                             const SingleStreamSettings& settings = {});

private:
    HttpClient& client_;
};

} // namespace flowget

#endif /* FLOWGET_SINGLE_STREAM_DOWNLOAD_H */

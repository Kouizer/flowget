#ifndef FLOWGET_DOWNLOAD_H
#define FLOWGET_DOWNLOAD_H

#include <flowget/http_client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace flowget {

// One download request, shared by the single-stream path and the segmented engine.
//
// `output_path` is UTF-8, not std::filesystem::path: on Windows a path built from a narrow
// std::string is interpreted in the ANSI code page and mangles Turkish/CJK/emoji names. The storage
// layer converts UTF-8 to UTF-16 internally, so the correct behaviour is the default (S3 decision,
// tasks/lessons.md).
struct DownloadOptions {
    std::string url;
    std::string output_path;
    std::string user_agent = "FlowGet/0.1.0";
    // Extra request headers (authorization, referer, cookie, ...), sent with the probe, every
    // segment, and the single-stream request.
    std::map<std::string, std::string> headers;
    // Concurrent connections for the segmented engine, clamped to [1, 64]. 1 means "single stream
    // without probing", which is also what a server without range support gets; that path resumes
    // from its own single-segment sidecar.
    int connections = 8;
    // When true (the default), a valid sidecar makes a segmented download continue from the bytes
    // already on disk. false deletes the sidecar and starts from zero.
    bool resume = true;
    // Called once per download, before the first body byte is written, after the resume decision.
    // The argument is the number of bytes already durable: 0 for a fresh download (including a
    // resume request that the server answered with 200), the checkpointed `done` for a resumed one,
    // and the full size for an output that was already complete. Must not throw.
    std::function<void(std::int64_t)> on_resume;
};

struct DownloadProgress {
    std::int64_t bytes_written = 0;
    // -1 when the server did not announce a length (chunked transfer).
    std::int64_t total_bytes = -1;

    // 0.0 when the total is unknown; otherwise 0.0-1.0.
    [[nodiscard]] double fraction() const;
};

// Runs on the thread that called download(). It must not throw.
using ProgressCallback = std::function<void(const DownloadProgress&)>;

struct DownloadSummary {
    // For a segmented download this is synthesised from the probe: status 206, content_length equal
    // to the file size, ETag/effective URL from the probe. On failure it carries the error of the
    // request that failed (or of the probe).
    HttpResult http;
    std::int64_t bytes_written = 0;
    // Set for failures outside the HTTP layer (file creation, disk write, path handling).
    std::string message;
    // Connections actually used: 0 for an empty resource or an already-complete output, 1 for a
    // single stream.
    int connections = 1;
    // True when the segmented attempt was abandoned and the file was fetched again as one stream.
    bool fallback = false;
    // Why the fallback happened ("server ignored Range", "Content-Range mismatch", ...).
    std::string fallback_reason;
    // True when a validated sidecar was used, including an output that was already complete.
    bool resumed = false;
    // Bytes that were already durable when the resumed transfer started (0 when not resumed).
    std::int64_t resumed_from_bytes = 0;
    // Why a sidecar was ignored, or why the attempt restarted from zero. Empty on a clean run.
    std::string resume_note;

    [[nodiscard]] bool ok() const {
        return http.ok() && message.empty();
    }
};

} // namespace flowget

#endif /* FLOWGET_DOWNLOAD_H */

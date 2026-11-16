#ifndef FLOWGET_SINGLE_STREAM_DOWNLOAD_H
#define FLOWGET_SINGLE_STREAM_DOWNLOAD_H

#include <flowget/download.h>
#include <flowget/http_client.h>

#include <atomic>

namespace flowget {

// M1/S1b: one connection, one file, no resume. It is the fallback for servers without range
// support (S4) and writes through IStorage so non-ASCII paths and partial writes behave the same as
// in the segmented engine.
class SingleStreamDownloader {
public:
    explicit SingleStreamDownloader(HttpClient& client);

    // Writes the response body to `options.output_path`, truncating an existing file. The header
    // fields of `options` other than url/user_agent/headers are ignored. On failure the partial
    // file is removed, because a single-stream transfer has no resume state to keep.
    DownloadSummary download(const DownloadOptions& options,
                             const ProgressCallback& on_progress = {},
                             const std::atomic<bool>* cancel = nullptr);

private:
    HttpClient& client_;
};

} // namespace flowget

#endif /* FLOWGET_SINGLE_STREAM_DOWNLOAD_H */

#ifndef FLOWGET_DOWNLOADER_H
#define FLOWGET_DOWNLOADER_H

#include <flowget/download.h>
#include <flowget/http_client.h>

#include <atomic>

namespace flowget {

// M1/S4 entry point: probes the server, plans byte ranges and downloads them over N connections.
// When the server cannot do ranges it falls back to one stream, and a semantic mismatch during the
// segmented attempt triggers the same fallback once. Transport errors and cancellation do not
// fall back: restarting a broken transfer from zero would hide the failure and waste bandwidth.
class Downloader {
public:
    explicit Downloader(HttpClient& client);

    // Writes to `options.output_path`, truncating an existing file. `options.connections` is
    // clamped to [1, 64]; 1 skips probing entirely.
    DownloadSummary download(const DownloadOptions& options,
                             const ProgressCallback& on_progress = {},
                             const std::atomic<bool>* cancel = nullptr);

private:
    HttpClient& client_;
};

} // namespace flowget

#endif /* FLOWGET_DOWNLOADER_H */

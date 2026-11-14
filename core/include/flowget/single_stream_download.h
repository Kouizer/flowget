#ifndef FLOWGET_SINGLE_STREAM_DOWNLOAD_H
#define FLOWGET_SINGLE_STREAM_DOWNLOAD_H

#include <flowget/http_client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace flowget {

struct DownloadOptions {
    std::string url;
    std::filesystem::path output_path;
    std::string user_agent = "FlowGet/0.1.0";
};

struct DownloadProgress {
    std::int64_t bytes_written = 0;
    // -1 when the server did not announce a length (chunked transfer).
    std::int64_t total_bytes = -1;

    // 0.0 when the total is unknown; otherwise 0.0-1.0.
    [[nodiscard]] double fraction() const;
};

// Callback runs on the thread that called `download()`. It must not throw.
using ProgressCallback = std::function<void(const DownloadProgress&)>;

struct DownloadSummary {
    HttpResult http;
    std::int64_t bytes_written = 0;
    // Set for failures that happen outside the HTTP layer (file creation, disk write).
    std::string message;

    [[nodiscard]] bool ok() const {
        return http.ok() && message.empty();
    }
};

// M1/S1b: one connection, one file, no resume. The segmented engine (S4) replaces this class for
// the real download path; it stays as the fallback for servers without range support.
class SingleStreamDownloader {
public:
    explicit SingleStreamDownloader(HttpClient& client);

    // Writes the response body to `options.output_path`, truncating an existing file. On failure
    // the partial file is removed, because a single-stream transfer has no resume state to keep.
    DownloadSummary download(const DownloadOptions& options,
                             const ProgressCallback& on_progress = {},
                             const std::atomic<bool>* cancel = nullptr);

private:
    HttpClient& client_;
};

} // namespace flowget

#endif /* FLOWGET_SINGLE_STREAM_DOWNLOAD_H */

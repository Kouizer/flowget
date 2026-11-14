#include <flowget/single_stream_download.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

namespace flowget {

double DownloadProgress::fraction() const {
    if (total_bytes <= 0) {
        return 0.0;
    }
    const double written = static_cast<double>(bytes_written);
    const double total = static_cast<double>(total_bytes);
    return std::clamp(written / total, 0.0, 1.0);
}

SingleStreamDownloader::SingleStreamDownloader(HttpClient& client) : client_(client) {}

DownloadSummary SingleStreamDownloader::download(const DownloadOptions& options,
                                                 const ProgressCallback& on_progress,
                                                 const std::atomic<bool>* cancel) {
    DownloadSummary summary;

    std::error_code filesystem_error;
    const std::filesystem::path parent = options.output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            summary.message =
                "cannot create directory " + parent.string() + ": " + filesystem_error.message();
            summary.http.error = HttpError::io;
            return summary;
        }
    }

    std::ofstream file{options.output_path, std::ios::binary | std::ios::trunc};
    if (!file) {
        summary.message = "cannot open " + options.output_path.string() + " for writing";
        summary.http.error = HttpError::io;
        return summary;
    }

    bool write_failed = false;
    std::int64_t bytes_written = 0;
    std::int64_t total_bytes = -1;
    auto last_report = std::chrono::steady_clock::now();

    const BodySink sink = [&](const std::byte* data, std::size_t size) {
        if (write_failed) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) {
            write_failed = true;
            return false;
        }
        bytes_written += static_cast<std::int64_t>(size);

        if (on_progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(100)) {
                last_report = now;
                on_progress(DownloadProgress{bytes_written, total_bytes});
            }
        }
        return true;
    };

    HttpRequest request;
    request.url = options.url;
    request.user_agent = options.user_agent;

    summary.http = client_.get(request, sink, cancel);
    total_bytes = summary.http.response.content_length;

    file.close();
    if (!file) {
        write_failed = true;
    }

    summary.bytes_written = bytes_written;
    if (write_failed) {
        summary.http.error = HttpError::io;
        summary.message = "failed while writing " + options.output_path.string();
    }

    if (on_progress && summary.http.ok()) {
        on_progress(DownloadProgress{bytes_written, total_bytes});
    }

    if (!summary.ok()) {
        std::error_code remove_error;
        std::filesystem::remove(options.output_path, remove_error);
    }

    return summary;
}

} // namespace flowget

#include <flowget/single_stream_download.h>
#include <flowget/storage.h>

#include "download_util.h"

#include <chrono>
#include <utility>

namespace flowget {

SingleStreamDownloader::SingleStreamDownloader(HttpClient& client) : client_(client) {}

DownloadSummary SingleStreamDownloader::download(const DownloadOptions& options,
                                                 const ProgressCallback& on_progress,
                                                 const std::atomic<bool>* cancel) {
    DownloadSummary summary;

    std::string filesystem_error;
    if (!detail::ensure_parent_directory(options.output_path, filesystem_error)) {
        summary.http.error = HttpError::io;
        summary.message = std::move(filesystem_error);
        return summary;
    }

    const auto storage = make_platform_storage();
    const StorageResult opened = storage->open(options.output_path, OpenMode::read_write_truncate);
    if (!opened.ok()) {
        summary.http.error = HttpError::io;
        summary.message =
            "cannot open " + options.output_path + " for writing: " +
            (opened.message.empty() ? storage_error_name(opened.error) : opened.message);
        return summary;
    }

    std::int64_t offset = 0;
    bool write_failed = false;
    std::string write_message;
    auto last_report = std::chrono::steady_clock::now();

    const BodySink sink = [&](const std::byte* data, std::size_t size) {
        if (write_failed) {
            return false;
        }
        std::size_t written = 0;
        const StorageResult result = storage->write_at(offset, data, size, written);
        if (!result.ok() || written != size) {
            write_failed = true;
            write_message =
                result.message.empty() ? storage_error_name(result.error) : result.message;
            return false;
        }
        offset += static_cast<std::int64_t>(written);

        if (on_progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(100)) {
                last_report = now;
                on_progress(DownloadProgress{offset, -1});
            }
        }
        return true;
    };

    HttpRequest request;
    request.url = options.url;
    request.user_agent = options.user_agent;
    request.headers = options.headers;

    summary.http = client_.get(request, sink, cancel);
    const std::int64_t total_bytes = summary.http.response.content_length;

    if (summary.http.ok()) {
        const StorageResult flushed = storage->flush();
        if (!flushed.ok()) {
            write_failed = true;
            write_message =
                flushed.message.empty() ? storage_error_name(flushed.error) : flushed.message;
        }
    }
    const StorageResult closed = storage->close();
    if (!closed.ok() && !write_failed) {
        write_failed = true;
        write_message = closed.message.empty() ? storage_error_name(closed.error) : closed.message;
    }

    summary.bytes_written = offset;
    if (write_failed) {
        summary.http.error = HttpError::io;
        summary.message =
            write_message.empty() ? "failed while writing " + options.output_path : write_message;
    }

    if (on_progress && summary.http.ok()) {
        on_progress(DownloadProgress{offset, total_bytes});
    }

    if (!summary.ok()) {
        std::string remove_error;
        static_cast<void>(detail::remove_file(options.output_path, remove_error));
    }

    return summary;
}

} // namespace flowget

#include <flowget/http_client.h>

#include "curl_common.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <curl/curl.h>

namespace flowget {
namespace {

struct RequestContext {
    const BodySink* sink = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    std::map<std::string, std::string> headers;
    std::int64_t bytes_received = 0;
};

std::size_t on_header(char* buffer, std::size_t size, std::size_t count, void* userdata) {
    auto* context = static_cast<RequestContext*>(userdata);
    const std::size_t total = size * count;
    const std::string_view line{buffer, total};

    // A new status line starts the headers of a new response (redirect chain): keep only the final
    // response's headers, because those describe the body we are writing.
    if (line.starts_with("HTTP/")) {
        context->headers.clear();
        return total;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return total;
    }

    const std::string key = detail::lowercase(detail::trim(line.substr(0, colon)));
    const std::string value{detail::trim(line.substr(colon + 1))};
    if (!key.empty() && context->headers.find(key) == context->headers.end()) {
        context->headers.emplace(key, value);
    }
    return total;
}

std::size_t on_body(char* buffer, std::size_t size, std::size_t count, void* userdata) {
    auto* context = static_cast<RequestContext*>(userdata);
    const std::size_t total = size * count;
    if (context->sink == nullptr) {
        return total;
    }
    // libcurl hands the body over as char*; BodySink takes std::byte* because the bytes are opaque.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C callback boundary
    if (!(*context->sink)(reinterpret_cast<const std::byte*>(buffer), total)) {
        return 0; // Returning a short count aborts the transfer (CURLE_WRITE_ERROR).
    }
    context->bytes_received += static_cast<std::int64_t>(total);
    return total;
}

int on_progress(void* userdata,
                curl_off_t /*dltotal*/,
                curl_off_t /*dlnow*/,
                curl_off_t /*ultotal*/,
                curl_off_t /*ulnow*/) {
    auto* context = static_cast<RequestContext*>(userdata);
    if (context->cancel != nullptr && context->cancel->load(std::memory_order_relaxed)) {
        return 1;
    }
    return 0;
}

class CurlTransport final : public HttpTransport {
public:
    CurlTransport() {
        detail::ensure_global_init();
    }

    HttpResult get(const HttpRequest& request,
                   const BodySink& sink,
                   const std::atomic<bool>* cancel) override {
        HttpResult result;
        RequestContext context;
        context.sink = &sink;
        context.cancel = cancel;

        const detail::MultiHandle multi{curl_multi_init()};
        const detail::EasyHandle easy{curl_easy_init()};
        if (!multi || !easy) {
            result.error = HttpError::internal;
            result.message = "libcurl handle allocation failed";
            return result;
        }

        std::optional<detail::HeaderList> outgoing_headers =
            detail::make_header_list(request.headers);
        if (!outgoing_headers.has_value()) {
            result.error = HttpError::internal;
            result.message = "failed to build request headers";
            return result;
        }

        std::array<char, CURL_ERROR_SIZE> error_buffer{};
        CURL* handle = easy.get();
        if (!detail::configure_easy(
                handle, request, error_buffer.data(), outgoing_headers->get())) {
            result.error = HttpError::internal;
            result.message = "invalid byte range";
            return result;
        }
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &context);
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context);

        if (curl_multi_add_handle(multi.get(), handle) != CURLM_OK) {
            result.error = HttpError::internal;
            result.message = "failed to register the transfer";
            return result;
        }

        int running = 0;
        CURLcode transfer_code = CURLE_OK;
        while (true) {
            const CURLMcode multi_code = curl_multi_perform(multi.get(), &running);
            if (multi_code != CURLM_OK) {
                result.error = HttpError::internal;
                result.message = curl_multi_strerror(multi_code);
                break;
            }
            if (running == 0) {
                break;
            }
            // 50 ms poll interval bounds cancellation latency without busy-waiting.
            static_cast<void>(curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr));
        }

        int queued = 0;
        while (const CURLMsg* message = curl_multi_info_read(multi.get(), &queued)) {
            if (message->msg == CURLMSG_DONE) {
                // libcurl's CURLMsg::data is a union by design; `result` is the documented member
                // after CURLMSG_DONE and has no variant equivalent.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): documented union member
                transfer_code = message->data.result;
            }
        }
        curl_multi_remove_handle(multi.get(), handle);

        const long status = [&] {
            long value = 0;
            static_cast<void>(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &value));
            return value;
        }();
        result.response.status = status;
        result.curl_code = static_cast<int>(transfer_code);

        curl_off_t content_length = -1;
        static_cast<void>(
            curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length));
        result.response.content_length = static_cast<std::int64_t>(content_length);

        char* effective_url = nullptr;
        static_cast<void>(curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url));
        if (effective_url != nullptr) {
            result.response.effective_url = effective_url;
        }

        result.response.etag = detail::header_value(context.headers, "etag");
        result.response.last_modified = detail::header_value(context.headers, "last-modified");
        result.response.accept_ranges = detail::header_value(context.headers, "accept-ranges");
        result.response.content_range = detail::header_value(context.headers, "content-range");
        result.response.content_type = detail::header_value(context.headers, "content-type");
        result.response.content_disposition =
            detail::header_value(context.headers, "content-disposition");

        const bool cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
        if (transfer_code == CURLE_OK) {
            if (status >= 400) {
                result.error = HttpError::http_status;
                result.message = "HTTP status " + std::to_string(status);
            }
            return result;
        }

        if (cancelled &&
            (transfer_code == CURLE_WRITE_ERROR || transfer_code == CURLE_ABORTED_BY_CALLBACK)) {
            result.error = HttpError::aborted;
            result.message = "transfer cancelled";
            return result;
        }

        result.error = detail::map_curl_error(transfer_code);
        if (error_buffer[0] != '\0') {
            result.message = error_buffer.data();
        } else {
            result.message = curl_easy_strerror(transfer_code);
        }
        return result;
    }
};

} // namespace

std::unique_ptr<HttpTransport> make_curl_transport() {
    return std::make_unique<CurlTransport>();
}

} // namespace flowget

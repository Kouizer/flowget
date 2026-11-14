#include <flowget/http_client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>

namespace flowget {
namespace {

// libcurl is initialized once per process; cleanup is deliberately skipped because handles may
// still exist during static destruction and the OS reclaims the memory at exit.
void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
}

struct MultiDeleter {
    void operator()(CURLM* handle) const {
        curl_multi_cleanup(handle);
    }
};

struct EasyDeleter {
    void operator()(CURL* handle) const {
        curl_easy_cleanup(handle);
    }
};

struct ListDeleter {
    void operator()(curl_slist* list) const {
        curl_slist_free_all(list);
    }
};

using MultiHandle = std::unique_ptr<CURLM, MultiDeleter>;
using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;
using HeaderList = std::unique_ptr<curl_slist, ListDeleter>;

std::string lowercase(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

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
    if (line.rfind("HTTP/", 0) == 0) {
        context->headers.clear();
        return total;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return total;
    }

    const std::string key = lowercase(trim(line.substr(0, colon)));
    const std::string value{trim(line.substr(colon + 1))};
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

HttpError map_curl_error(CURLcode code) {
    switch (code) {
    case CURLE_OK:
        return HttpError::none;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return HttpError::invalid_url;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
        return HttpError::connect;
    case CURLE_OPERATION_TIMEDOUT:
        return HttpError::timeout;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
        return HttpError::tls;
    case CURLE_ABORTED_BY_CALLBACK:
    case CURLE_WRITE_ERROR:
        return HttpError::aborted;
    case CURLE_OUT_OF_MEMORY:
        return HttpError::internal;
    default:
        return HttpError::internal;
    }
}

std::string header_value(const std::map<std::string, std::string>& headers, std::string_view name) {
    const auto iterator = headers.find(std::string{name});
    return iterator == headers.end() ? std::string{} : iterator->second;
}

class CurlTransport final : public HttpTransport {
public:
    CurlTransport() {
        ensure_global_init();
    }

    HttpResult get(const HttpRequest& request,
                   const BodySink& sink,
                   const std::atomic<bool>* cancel) override {
        HttpResult result;
        RequestContext context;
        context.sink = &sink;
        context.cancel = cancel;

        MultiHandle multi{curl_multi_init()};
        EasyHandle easy{curl_easy_init()};
        if (!multi || !easy) {
            result.error = HttpError::internal;
            result.message = "libcurl handle allocation failed";
            return result;
        }

        HeaderList outgoing_headers;
        for (const auto& [name, value] : request.headers) {
            const std::string line = name + ": " + value;
            curl_slist* appended = curl_slist_append(outgoing_headers.get(), line.c_str());
            if (appended == nullptr) {
                result.error = HttpError::internal;
                result.message = "failed to build request headers";
                return result;
            }
            outgoing_headers.release();
            outgoing_headers.reset(appended);
        }

        char error_buffer[CURL_ERROR_SIZE] = {};
        CURL* handle = easy.get();
        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
        // Byte offsets must refer to the payload, not to a compressed stream (ADR-0001).
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, request.user_agent.c_str());
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &context);
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context);
        if (outgoing_headers) {
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, outgoing_headers.get());
        }

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

        result.response.etag = header_value(context.headers, "etag");
        result.response.last_modified = header_value(context.headers, "last-modified");
        result.response.accept_ranges = header_value(context.headers, "accept-ranges");
        result.response.content_type = header_value(context.headers, "content-type");
        result.response.content_disposition = header_value(context.headers, "content-disposition");

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

        result.error = map_curl_error(transfer_code);
        if (error_buffer[0] != '\0') {
            result.message = error_buffer;
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

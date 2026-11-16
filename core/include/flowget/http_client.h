#ifndef FLOWGET_HTTP_CLIENT_H
#define FLOWGET_HTTP_CLIENT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace flowget {

// Stable error taxonomy of the HTTP layer. The C ABI maps these onto `flowget_status`; nothing in
// core throws across a boundary (ADR-0002).
enum class HttpError {
    none = 0,
    invalid_url,
    connect,
    tls,
    timeout,
    aborted,
    http_status,
    io,
    internal,
};

const char* http_error_name(HttpError error);

enum class HttpMethod {
    get = 0,
    // HEAD: metadata only. Some servers answer 403/405 to HEAD (signed URLs that only allow GET) or
    // never answer at all, so callers must be able to fall back to GET without treating that as a
    // failure.
    head,
};

struct HttpRequest {
    std::string url;
    HttpMethod method = HttpMethod::get;
    std::string user_agent = "FlowGet/0.1.0";
    // Extra request headers, e.g. "referer", "cookie", "authorization".
    std::map<std::string, std::string> headers;
    bool follow_redirects = true;
    // Hard limit for the whole transfer, redirects included. 0 keeps the transport defaults
    // (connect timeout + low-speed abort). Used by probing, where a stalled server must not block.
    std::int64_t timeout_ms = 0;
    // Optional byte range: `range_length` bytes starting at `range_start`. Both -1 means "no Range
    // header". The segmented engine sets one range per connection; a ranged request is pinned to
    // HTTP/1.1 on a fresh connection.
    std::int64_t range_start = -1;
    std::int64_t range_length = -1;
};

struct HttpResponse {
    long status = 0;
    // -1 when the server did not announce a length (chunked or connection-close framing).
    std::int64_t content_length = -1;
    std::string effective_url;
    std::string etag;
    std::string last_modified;
    std::string accept_ranges;
    // "Content-Range" of a 206/416 response, e.g. "bytes 0-0/12345" or "bytes */0"; empty when the
    // header was absent. Parsing lives in http_range.h.
    std::string content_range;
    std::string content_type;
    std::string content_disposition;
};

// Body callback: returns false to abort the transfer. Bytes are delivered in arrival order and are
// only valid for the duration of the call.
using BodySink = std::function<bool(const std::byte* data, std::size_t size)>;

struct HttpResult {
    HttpError error = HttpError::none;
    // Raw libcurl code, kept for logs and bug reports. 0 when the request was never started.
    int curl_code = 0;
    std::string message;
    HttpResponse response;

    [[nodiscard]] bool ok() const {
        return error == HttpError::none;
    }
};

// The transport is an internal interface so that the underlying stack can be replaced without
// touching the download engine (ADR-0001). v1 ships exactly one implementation (libcurl multi).
class HttpTransport {
public:
    HttpTransport() = default;
    virtual ~HttpTransport() = default;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = delete;
    HttpTransport& operator=(HttpTransport&&) = delete;

    // Performs one request synchronously on the calling thread. `cancel` is polled; when it turns
    // true the transfer aborts with HttpError::aborted within ~50 ms.
    virtual HttpResult
    get(const HttpRequest& request, const BodySink& sink, const std::atomic<bool>* cancel) = 0;
};

std::unique_ptr<HttpTransport> make_curl_transport();

// Thin facade over the transport; owns it. One instance can serve any number of sequential
// requests. Constructing it initializes libcurl once per process.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    HttpResult get(const HttpRequest& request,
                   const BodySink& sink,
                   const std::atomic<bool>* cancel = nullptr);

private:
    std::unique_ptr<HttpTransport> transport_;
};

} // namespace flowget

#endif /* FLOWGET_HTTP_CLIENT_H */

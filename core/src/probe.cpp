#include <flowget/http_filename.h>
#include <flowget/http_range.h>
#include <flowget/probe.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace flowget {
namespace {

constexpr const char* kFallbackFilename = "download.bin";
// libcurl delivers the body in chunks; this keeps a server that ignores `Range` from streaming a
// whole file into memory, without affecting the 1-byte 206 path.
constexpr std::int64_t kDefaultBodyLimit = 64 * 1024;

HttpRequest make_request(const ProbeOptions& options, HttpMethod method) {
    HttpRequest request;
    request.url = options.url;
    request.method = method;
    request.user_agent = options.user_agent;
    request.headers = options.headers;
    return request;
}

// Later answers win: the ranged GET describes the same resource right before the download, so its
// headers are fresher than the HEAD answer.
void apply_headers(ProbeResult& result, const HttpResponse& response) {
    if (!response.etag.empty()) {
        result.etag = response.etag;
    }
    if (!response.last_modified.empty()) {
        result.last_modified = response.last_modified;
    }
    if (!response.accept_ranges.empty()) {
        result.accept_ranges = response.accept_ranges;
    }
    if (!response.content_type.empty()) {
        result.content_type = response.content_type;
    }
    if (!response.content_disposition.empty()) {
        result.content_disposition = response.content_disposition;
    }
}

std::string effective_url_or(const HttpResponse& response, const ProbeOptions& options) {
    return response.effective_url.empty() ? options.url : response.effective_url;
}

} // namespace

const char* range_support_name(RangeSupport support) {
    switch (support) {
    case RangeSupport::unknown:
        return "unknown";
    case RangeSupport::supported:
        return "supported";
    case RangeSupport::ignored:
        return "ignored";
    case RangeSupport::unsatisfiable:
        return "unsatisfiable";
    }
    return "unknown";
}

ProbeResult
probe(HttpClient& client, const ProbeOptions& options, const std::atomic<bool>* cancel) {
    ProbeResult result;
    std::int64_t head_length = -1;

    // HEAD is an optimisation, never a requirement: signed URLs answer 403 to it, some servers
    // stall or drop it. Any failure is ignored and the ranged GET below still decides the probe
    // outcome.
    if (options.head_timeout_ms > 0) {
        HttpRequest head_request = make_request(options, HttpMethod::head);
        head_request.timeout_ms = options.head_timeout_ms;
        const HttpResult head = client.get(head_request, {}, cancel);
        if (head.ok() && head.response.status < 400) {
            result.head_used = true;
            apply_headers(result, head.response);
            head_length = head.response.content_length;
        }
        const bool cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
        if (cancelled) {
            result.error = HttpError::aborted;
            result.message = "probe cancelled";
            result.effective_url = options.url;
            return result;
        }
    }

    // The ranged GET is the authoritative request. The sink accepts a bounded prefix and then
    // returns false, which aborts the transfer with CURLE_WRITE_ERROR; that abort is expected and
    // handled below, since it is how a server without range support is detected cheaply.
    std::int64_t accepted = 0;
    const std::int64_t limit = std::max<std::int64_t>(
        options.body_limit_bytes > 0 ? options.body_limit_bytes : kDefaultBodyLimit, 1);
    const BodySink sink = [&accepted, limit](const std::byte* /*data*/, std::size_t size) {
        accepted += static_cast<std::int64_t>(size);
        return accepted <= limit;
    };

    HttpRequest range_request = make_request(options, HttpMethod::get);
    range_request.headers["Range"] = "bytes=0-0";
    range_request.timeout_ms = options.range_timeout_ms;

    const HttpResult ranged = client.get(range_request, sink, cancel);
    const bool cancelled = cancel != nullptr && cancel->load(std::memory_order_relaxed);
    // The sink only fails after the limit, so a non-cancelled write abort means "the server ignored
    // Range and we stopped it", not a real error.
    const bool sink_aborted = ranged.error == HttpError::aborted && !cancelled && accepted > limit;
    // 416 is a meaningful answer (the representation is empty or 0-0 lies outside it). The
    // transport reports every 4xx as http_status, so unwrap this one case here.
    const bool range_unsatisfiable =
        ranged.error == HttpError::http_status && ranged.response.status == 416;

    if (!ranged.ok() && !sink_aborted && !range_unsatisfiable) {
        result.error = ranged.error;
        result.message = ranged.message.empty() ? http_error_name(ranged.error) : ranged.message;
        result.status = ranged.response.status;
        result.effective_url = effective_url_or(ranged.response, options);
        return result;
    }

    const HttpResponse& response = ranged.response;
    result.status = response.status;
    apply_headers(result, response);
    result.effective_url = effective_url_or(response, options);

    if (response.status >= 400 && response.status != 416) {
        result.error = HttpError::http_status;
        result.message = "HTTP status " + std::to_string(response.status);
        return result;
    }

    switch (response.status) {
    case 206:
        result.range_support = RangeSupport::supported;
        break;
    case 200:
        result.range_support = RangeSupport::ignored;
        break;
    case 416:
        result.range_support = RangeSupport::unsatisfiable;
        break;
    default:
        result.range_support = RangeSupport::unknown;
        break;
    }

    // A 206 announces one byte through Content-Length, so the real size can only come from
    // Content-Range (or, failing that, from HEAD).
    const std::optional<ContentRange> range = parse_content_range(response.content_range);
    std::int64_t total = -1;
    if (response.status == 200) {
        total = response.content_length;
    }
    if (total < 0 && range.has_value()) {
        total = range->total;
    }
    if (total < 0) {
        total = head_length;
    }
    result.content_length = total;

    result.suggested_filename = filename_from_content_disposition(result.content_disposition);
    if (result.suggested_filename.empty()) {
        result.suggested_filename = filename_from_url(result.effective_url);
    }
    if (result.suggested_filename.empty()) {
        result.suggested_filename = kFallbackFilename;
    }
    return result;
}

} // namespace flowget

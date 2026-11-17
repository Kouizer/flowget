#ifndef FLOWGET_PROBE_H
#define FLOWGET_PROBE_H

#include <flowget/http_client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace flowget {

// What a `Range: bytes=0-0` request told us about the server. This is the only input the segment
// planner needs, so it is derived from the actual response rather than from `Accept-Ranges` (which
// some servers send without honouring it).
enum class RangeSupport {
    unknown = 0,
    // 206 Partial Content: ranges work, segments can be planned.
    supported,
    // 200 OK to a ranged request: the header was ignored, download as one stream.
    ignored,
    // 416 Range Not Satisfiable: ranges work but 0-0 is outside the representation (empty file).
    unsatisfiable,
};

const char* range_support_name(RangeSupport support);

struct ProbeOptions {
    std::string url;
    std::string user_agent = "FlowGet/0.1.0";
    std::map<std::string, std::string> headers;
    // The optional HEAD request only enriches metadata (it is the cheapest way to learn the size of
    // a server that answers ranges but omits Content-Range). 0 disables it; a rejected
    // (403/405/501), stalled or otherwise failed HEAD is skipped silently, because the ranged GET
    // is authoritative.
    std::int64_t head_timeout_ms = 5000;
    // Hard limit for the ranged GET as a whole. 0 keeps the transport defaults.
    std::int64_t range_timeout_ms = 0;
    // Body bytes the ranged GET may deliver before the sink aborts it. Only reached when the server
    // ignores `Range` and starts streaming the whole file; a 206 answer sends a single byte.
    std::int64_t body_limit_bytes = std::int64_t{64} * 1024;
};

struct ProbeResult {
    HttpError error = HttpError::none;
    std::string message;
    long status = 0;
    // Total size in bytes; -1 when neither Content-Range nor a HEAD answered with a length.
    std::int64_t content_length = -1;
    RangeSupport range_support = RangeSupport::unknown;
    std::string etag;
    std::string last_modified;
    std::string accept_ranges;
    std::string content_type;
    std::string content_disposition;
    // Content-Disposition name, else the URL's last segment, else "download.bin".
    std::string suggested_filename;
    std::string effective_url;
    // True when the optional HEAD answered and contributed metadata.
    bool head_used = false;

    [[nodiscard]] bool ok() const {
        return error == HttpError::none;
    }
};

// Probes `options.url` without downloading it: HEAD (optional) followed by a ranged GET whose body
// is discarded. The ranged GET decides `range_support`; a 200/206/416 answer is success, transport
// failures and other HTTP error statuses are reported through `error`.
ProbeResult
probe(HttpClient& client, const ProbeOptions& options, const std::atomic<bool>* cancel = nullptr);

} // namespace flowget

#endif /* FLOWGET_PROBE_H */

#ifndef FLOWGET_CURL_COMMON_H
#define FLOWGET_CURL_COMMON_H

// Internal glue shared by the single-request transport and the segmented download engine. It is not
// installed: libcurl types must not leak into public headers (ADR-0001).

#include <flowget/http_client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <curl/curl.h>

namespace flowget::detail {

// libcurl is initialized once per process; cleanup is deliberately skipped because handles may
// still exist during static destruction and the OS reclaims the memory at exit.
void ensure_global_init();

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

// 256 KiB receive buffer: one libcurl read covers a full 256 KiB segment minimum, so a 64 MiB/s
// download does not pay for four thousand 16 KiB callbacks per second. Larger buffers would raise
// per-connection memory without changing throughput (the write syscall budget stays the same).
constexpr long kReceiveBufferSize = long{256} * 1024;

// Stable mapping from libcurl codes to the public HttpError taxonomy. Shared so a segment failure
// and a single-stream failure classify identically.
HttpError map_curl_error(CURLcode code);

// Small string helpers shared by the header parsers of both transfer paths.
std::string lowercase(std::string_view text);
std::string_view trim(std::string_view text);

std::string header_value(const std::map<std::string, std::string>& headers, std::string_view name);

// Copies `headers` into a curl_slist. An empty input yields an engaged but null list. std::nullopt
// means a memory allocation failed and the caller must report HttpError::internal.
std::optional<HeaderList> make_header_list(const std::map<std::string, std::string>& headers);

// Applies every setting FlowGet transfers share (timeouts, identity encoding, user agent, 256 KiB
// buffer, ...). Returns false when the request itself is invalid, e.g. a negative range.
//
// A ranged request is pinned to HTTP/1.1 and opens a fresh connection: HTTP/2 multiplexing would
// put several segments on one TCP connection, where head-of-line blocking and a shared congestion
// window erase the point of parallel ranges, and an idle reused connection would serialise
// segments.
bool configure_easy(CURL* handle,
                    const HttpRequest& request,
                    char* error_buffer,
                    curl_slist* outgoing_headers);

} // namespace flowget::detail

#endif /* FLOWGET_CURL_COMMON_H */

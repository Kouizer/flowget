#include "curl_common.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>

namespace flowget::detail {
namespace {

// Converts request.timeout_ms into a `long` without overflowing it on LLP64, where long is 32-bit.
long clamp_timeout(std::int64_t timeout_ms) {
    constexpr auto kMaxTimeout = static_cast<std::int64_t>(std::numeric_limits<long>::max());
    return static_cast<long>(std::min<std::int64_t>(timeout_ms, kMaxTimeout));
}

} // namespace

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

void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
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
    default:
        return HttpError::internal;
    }
}

std::string header_value(const std::map<std::string, std::string>& headers, std::string_view name) {
    const auto iterator = headers.find(lowercase(name));
    return iterator == headers.end() ? std::string{} : iterator->second;
}

std::optional<HeaderList> make_header_list(const std::map<std::string, std::string>& headers) {
    HeaderList list;
    for (const auto& [name, value] : headers) {
        std::string line{name};
        line.append(": ").append(value);
        // curl_slist_append takes ownership only on success; release first so a failed append frees
        // whatever was already built instead of leaking it.
        curl_slist* head = list.release();
        curl_slist* appended = curl_slist_append(head, line.c_str());
        if (appended == nullptr) {
            list.reset(head);
            return std::nullopt;
        }
        list.reset(appended);
    }
    return list;
}

bool configure_easy(CURL* handle,
                    const HttpRequest& request,
                    char* error_buffer,
                    curl_slist* outgoing_headers) {
    const bool ranged = request.range_start >= 0 || request.range_length >= 0;
    if (ranged) {
        if (request.range_start < 0 || request.range_length <= 0 ||
            request.range_start >
                std::numeric_limits<std::int64_t>::max() - (request.range_length - 1)) {
            return false;
        }
    }

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    if (request.method == HttpMethod::head) {
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    }
    if (request.timeout_ms > 0) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, clamp_timeout(request.timeout_ms));
    }
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
    curl_easy_setopt(handle, CURLOPT_BUFFERSIZE, kReceiveBufferSize);
    if (outgoing_headers != nullptr) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, outgoing_headers);
    }

    if (ranged) {
        // HTTP/1.1 + a fresh connection per segment: HTTP/2 would multiplex the ranges onto one
        // connection and a reused idle connection would make them sequential. See curl_common.h.
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 1L);
        const std::string range = std::to_string(request.range_start) + "-" +
                                  std::to_string(request.range_start + request.range_length - 1);
        curl_easy_setopt(handle, CURLOPT_RANGE, range.c_str());
    }
    return true;
}

} // namespace flowget::detail

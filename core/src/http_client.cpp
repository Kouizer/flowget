#include <flowget/http_client.h>

#include <utility>

namespace flowget {

const char* http_error_name(HttpError error) {
    switch (error) {
    case HttpError::none:
        return "none";
    case HttpError::invalid_url:
        return "invalid_url";
    case HttpError::connect:
        return "connect";
    case HttpError::tls:
        return "tls";
    case HttpError::timeout:
        return "timeout";
    case HttpError::aborted:
        return "aborted";
    case HttpError::http_status:
        return "http_status";
    case HttpError::io:
        return "io";
    case HttpError::internal:
        return "internal";
    }
    return "internal";
}

HttpClient::HttpClient() : transport_(make_curl_transport()) {}

HttpClient::~HttpClient() = default;

HttpClient::HttpClient(HttpClient&&) noexcept = default;

HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

HttpResult
HttpClient::get(const HttpRequest& request, const BodySink& sink, const std::atomic<bool>* cancel) {
    return transport_->get(request, sink, cancel);
}

} // namespace flowget

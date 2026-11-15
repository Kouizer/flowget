#ifndef FLOWGET_TEST_HTTP_TEST_SERVER_H
#define FLOWGET_TEST_HTTP_TEST_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace flowget::test {

struct Request {
    std::string method;
    std::string target;
    // Header names are lowercased; values are trimmed.
    std::map<std::string, std::string> headers;
};

struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    // Chunked framing instead of Content-Length.
    bool chunked = false;
    std::size_t chunk_size = 16 * 1024;
    // 0 = full speed. Otherwise the body is paced to roughly this many bytes per second, which
    // makes per-connection throughput measurable and cancellation observable.
    std::size_t throttle_bytes_per_second = 0;
    // >0: close the connection abruptly after this many body bytes (a dropped transfer).
    std::size_t drop_after_bytes = 0;
    // Serve `Range: bytes=a-b` / `bytes=a-` as 206 + Content-Range, and 416 when the range starts
    // at or past the end of the body. When false the header is ignored and the full body goes out
    // with 200, which is how a server without range support behaves.
    bool honor_range = false;
    // Accept the request but never write a response. The connection is closed by the client (or by
    // server teardown), which makes "GET is fine, HEAD stalls" reproducible in one handler.
    bool stall = false;
};

// Minimal blocking HTTP/1.1 server for tests: loopback only, one thread per connection, one request
// per connection (`Connection: close`). It exists so that failure modes (chunked framing, throttled
// connections, dropped transfers, bogus headers) can be produced deterministically without an
// external process or network access.
class HttpTestServer {
public:
    using Handler = std::function<Response(const Request&)>;

    explicit HttpTestServer(Handler handler);
    ~HttpTestServer();

    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;
    HttpTestServer(HttpTestServer&&) = delete;
    HttpTestServer& operator=(HttpTestServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string url(std::string_view path = "/") const;
    [[nodiscard]] std::size_t request_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flowget::test

#endif /* FLOWGET_TEST_HTTP_TEST_SERVER_H */

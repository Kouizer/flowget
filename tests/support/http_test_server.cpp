// windows.h (pulled in by winsock2.h) defines min/max as macros, which breaks std::min/std::max.
// NOMINMAX must be set before the first Windows header is included.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "http_test_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace flowget::test {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

// Winsock spells every length/return value as `int`: send()/recv() return int, buffer sizes and
// sockaddr lengths are int. Keeping the platform types in one place stops the POSIX `ssize_t`
// results from being narrowed into `int` (which truncates and trips -Wconversion).
using SocketLength = int;
using SocketIoSize = int;
using IoResult = int;

int send_flags() {
    return 0; // MSG_NOSIGNAL does not exist on Windows; there is no SIGPIPE either.
}

int close_socket(SocketHandle socket) {
    return ::closesocket(socket);
}

void shutdown_socket(SocketHandle socket) {
    static_cast<void>(::shutdown(socket, SD_BOTH));
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

// POSIX: send()/recv() take a size_t length and return ssize_t (bytes, or -1); sockaddr lengths
// use socklen_t.
using SocketLength = socklen_t;
using SocketIoSize = std::size_t;
using IoResult = ssize_t;

int send_flags() {
    // A peer may close the connection between two body pieces (dropped/cancelled transfers).
    // Without MSG_NOSIGNAL the resulting SIGPIPE would kill the whole test binary instead of
    // failing send().
    return MSG_NOSIGNAL;
}

int close_socket(SocketHandle socket) {
    return ::close(socket);
}

void shutdown_socket(SocketHandle socket) {
    static_cast<void>(::shutdown(socket, SHUT_RDWR));
}
#endif

void ensure_winsock() {
#ifdef _WIN32
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data{};
        static_cast<void>(WSAStartup(MAKEWORD(2, 2), &data));
    });
#endif
}

void set_io_timeout(SocketHandle socket) {
    // Every blocking call gives up after a few seconds so that a misbehaving test cannot hang the
    // suite, and so that a server can always be torn down.
#ifdef _WIN32
    const DWORD timeout_ms = 5000;
    static_cast<void>(::setsockopt(socket,
                                   SOL_SOCKET,
                                   SO_RCVTIMEO,
                                   reinterpret_cast<const char*>(&timeout_ms),
                                   sizeof(timeout_ms)));
    static_cast<void>(::setsockopt(socket,
                                   SOL_SOCKET,
                                   SO_SNDTIMEO,
                                   reinterpret_cast<const char*>(&timeout_ms),
                                   sizeof(timeout_ms)));
#else
    const timeval timeout{5, 0};
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
#endif
}

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

bool send_all(SocketHandle socket, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const std::size_t remaining = size - sent;
        const auto piece = static_cast<SocketIoSize>(std::min<std::size_t>(remaining, 64 * 1024));
        const IoResult written = ::send(socket, data + sent, piece, send_flags());
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

std::string reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 302:
        return "Found";
    case 404:
        return "Not Found";
    case 416:
        return "Range Not Satisfiable";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

bool parse_decimal(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

// Implements the subset of RFC 9110 range handling the probe uses: a single `bytes=a-b` or
// `bytes=a-` range. Anything else (suffix ranges, multiple ranges, other units) is ignored and the
// full body is served, mirroring how a minimal server behaves.
void apply_range(const Request& request, Response& response) {
    const auto iterator = request.headers.find("range");
    if (iterator == request.headers.end()) {
        return;
    }
    constexpr std::string_view kPrefix = "bytes=";
    const std::string_view value = iterator->second;
    if (value.rfind(kPrefix, 0) != 0) {
        return;
    }
    const std::string_view specification = value.substr(kPrefix.size());
    if (specification.find(',') != std::string_view::npos) {
        return;
    }
    const std::size_t dash = specification.find('-');
    if (dash == std::string_view::npos) {
        return;
    }

    std::uint64_t first = 0;
    if (!parse_decimal(trim(specification.substr(0, dash)), first)) {
        return; // suffix ranges are deliberately unsupported by the fixture
    }

    const std::uint64_t size = static_cast<std::uint64_t>(response.body.size());
    if (first >= size) {
        response.status = 416;
        response.headers["Content-Range"] = "bytes */" + std::to_string(size);
        response.body.clear();
        return;
    }

    const std::string_view last_text = trim(specification.substr(dash + 1));
    std::uint64_t last = size - 1;
    if (!last_text.empty()) {
        if (!parse_decimal(last_text, last)) {
            return;
        }
        last = std::min(last, size - 1);
    }

    response.status = 206;
    response.headers["Content-Range"] =
        "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size);
    response.body = response.body.substr(static_cast<std::size_t>(first),
                                         static_cast<std::size_t>(last - first + 1));
}

bool read_request(SocketHandle socket, Request& request) {
    std::string buffer;
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        if (buffer.size() > 64 * 1024) {
            return false;
        }
        const IoResult received =
            ::recv(socket, chunk, static_cast<SocketIoSize>(sizeof(chunk)), 0);
        if (received <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
    }

    const std::size_t line_end = buffer.find("\r\n");
    const std::string_view request_line{buffer.data(), line_end};
    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return false;
    }
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return false;
    }
    request.method = std::string{request_line.substr(0, first_space)};
    request.target =
        std::string{request_line.substr(first_space + 1, second_space - first_space - 1)};

    std::size_t position = line_end + 2;
    while (position < buffer.size()) {
        const std::size_t end = buffer.find("\r\n", position);
        if (end == std::string::npos || end == position) {
            break;
        }
        const std::string_view line{buffer.data() + position, end - position};
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            request.headers[lowercase(trim(line.substr(0, colon)))] =
                std::string{trim(line.substr(colon + 1))};
        }
        position = end + 2;
    }
    return true;
}

bool write_head(SocketHandle socket, const Response& response) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       reason_phrase(response.status) + "\r\n";
    head += "Server: FlowGetTestServer/1.0\r\n";
    head += "Connection: close\r\n";

    for (const auto& [name, value] : response.headers) {
        head += name + ": " + value + "\r\n";
    }
    if (response.chunked) {
        head += "Transfer-Encoding: chunked\r\n";
    } else {
        head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    }
    head += "\r\n";
    return send_all(socket, head.data(), head.size());
}

bool write_body(SocketHandle socket, const Response& response) {
    const std::size_t limit = response.drop_after_bytes == 0
                                  ? response.body.size()
                                  : std::min(response.body.size(), response.drop_after_bytes);
    const std::size_t piece_size = response.chunked ? response.chunk_size : 64 * 1024;

    std::size_t offset = 0;
    while (offset < limit) {
        const std::size_t count = std::min(piece_size, limit - offset);

        if (response.chunked) {
            char size_line[32] = {};
            const int length = std::snprintf(
                size_line, sizeof(size_line), "%llx\r\n", static_cast<unsigned long long>(count));
            if (length <= 0 || !send_all(socket, size_line, static_cast<std::size_t>(length))) {
                return false;
            }
        }

        if (!send_all(socket, response.body.data() + offset, count)) {
            return false;
        }
        if (response.chunked && !send_all(socket, "\r\n", 2)) {
            return false;
        }

        offset += count;

        if (response.throttle_bytes_per_second > 0) {
            const auto bytes = static_cast<std::int64_t>(count);
            const auto rate = static_cast<std::int64_t>(response.throttle_bytes_per_second);
            std::this_thread::sleep_for(std::chrono::nanoseconds(bytes * 1'000'000'000LL / rate));
        }
    }

    if (response.drop_after_bytes > 0 && response.drop_after_bytes < response.body.size()) {
        // Abrupt close without the terminating chunk / without all bytes.
        shutdown_socket(socket);
        return true;
    }

    if (response.chunked) {
        return send_all(socket, "0\r\n\r\n", 5);
    }
    return true;
}

} // namespace

struct HttpTestServer::Impl {
    explicit Impl(Handler handler_in) : handler(std::move(handler_in)) {}

    Handler handler;
    SocketHandle listener = kInvalidSocket;
    std::uint16_t port = 0;
    std::atomic<std::size_t> requests{0};
    std::atomic<bool> stopping{false};
    std::jthread accept_thread;
    std::mutex mutex;
    std::vector<std::jthread> workers;
    std::set<SocketHandle> active;

    void accept_loop(std::stop_token token) {
        while (!token.stop_requested() && !stopping.load()) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            SocketLength address_length = static_cast<SocketLength>(sizeof(address));
            const SocketHandle client =
                ::accept(listener, reinterpret_cast<sockaddr*>(&address), &address_length);
            if (client == kInvalidSocket) {
                if (stopping.load()) {
                    return;
                }
                continue;
            }

            set_io_timeout(client);
            std::lock_guard<std::mutex> guard(mutex);
            active.insert(client);
            workers.emplace_back([this, client] { handle_connection(client); });
        }
    }

    void handle_connection(SocketHandle client) {
        Request request;
        if (read_request(client, request)) {
            requests.fetch_add(1);

            Handler local_handler;
            {
                std::lock_guard<std::mutex> guard(mutex);
                local_handler = handler;
            }

            Response served = local_handler(request);
            if (served.stall) {
                // Read until the peer gives up: no status line, no headers, no body. Server
                // teardown shuts the socket down, so this returns during destruction as well.
                char byte = 0;
                static_cast<void>(::recv(client, &byte, static_cast<SocketIoSize>(1), 0));
            } else {
                if (served.honor_range) {
                    apply_range(request, served);
                }
                if (write_head(client, served) && request.method != "HEAD") {
                    static_cast<void>(write_body(client, served));
                }
            }
        }

        static_cast<void>(close_socket(client));
        std::lock_guard<std::mutex> guard(mutex);
        active.erase(client);
    }

    ~Impl() {
        stopping = true;
        accept_thread.request_stop();
        if (listener != kInvalidSocket) {
            shutdown_socket(listener);
            static_cast<void>(close_socket(listener));
            listener = kInvalidSocket;
        }
        if (accept_thread.joinable()) {
            accept_thread.join();
        }

        std::vector<std::jthread> pending;
        {
            std::lock_guard<std::mutex> guard(mutex);
            for (const SocketHandle socket : active) {
                shutdown_socket(socket);
            }
            pending = std::move(workers);
        }

        for (std::jthread& worker : pending) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

HttpTestServer::HttpTestServer(Handler handler)
    : impl_(std::make_unique<Impl>(std::move(handler))) {
    ensure_winsock();

    Impl& impl = *impl_;
    impl.listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl.listener == kInvalidSocket) {
        throw std::runtime_error("test server: socket() failed");
    }

    const int reuse = 1;
    static_cast<void>(::setsockopt(impl.listener,
                                   SOL_SOCKET,
                                   SO_REUSEADDR,
                                   reinterpret_cast<const char*>(&reuse),
                                   sizeof(reuse)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(impl.listener,
               reinterpret_cast<sockaddr*>(&address),
               static_cast<SocketLength>(sizeof(address))) != 0) {
        static_cast<void>(close_socket(impl.listener));
        throw std::runtime_error("test server: bind() failed");
    }
    if (::listen(impl.listener, 16) != 0) {
        static_cast<void>(close_socket(impl.listener));
        throw std::runtime_error("test server: listen() failed");
    }

    SocketLength length = static_cast<SocketLength>(sizeof(address));
    if (::getsockname(impl.listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        static_cast<void>(close_socket(impl.listener));
        throw std::runtime_error("test server: getsockname() failed");
    }
    impl.port = ntohs(address.sin_port);

    impl.accept_thread = std::jthread([&impl](std::stop_token token) { impl.accept_loop(token); });
}

HttpTestServer::~HttpTestServer() = default;

std::uint16_t HttpTestServer::port() const {
    return impl_->port;
}

std::string HttpTestServer::url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(impl_->port) + std::string{path};
}

std::size_t HttpTestServer::request_count() const {
    return impl_->requests.load();
}

} // namespace flowget::test

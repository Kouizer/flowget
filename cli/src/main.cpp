#include <flowget/build_info.h>
#include <flowget/download.h>
#include <flowget/downloader.h>
#include <flowget/flowget.h>
#include <flowget/http_client.h>
#include <flowget/probe.h>
#include <flowget/segment_planner.h>

#include "console_cancel.h"

#include <cctype>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cstddef>

#include <windows.h>
#endif

namespace {

constexpr std::string_view kProgramName = "flowget-cli";

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: %.*s <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  get <url> -o <file>   Download one file (segmented when the server allows it)\n"
                 "  probe <url>           Report size, range support and metadata, no download\n"
                 "  --version, -V         Print version information and exit\n"
                 "  --help, -h            Print this help and exit\n"
                 "\n"
                 "Get options:\n"
                 "  -o, --output <file>   Destination path (UTF-8)\n"
                 "  --connections <n>     Concurrent connections, 1-64 (default: 8)\n"
                 "                        1 skips probing and downloads over a single stream\n"
                 "  --no-resume           Ignore a .flowget sidecar and download from zero\n"
                 "  --user-agent <ua>     User-Agent header (default: FlowGet/0.1.0)\n"
                 "  --header \"Name: v\"    Extra request header; repeatable\n"
                 "\n"
                 "Probe options:\n"
                 "  --user-agent <ua>     User-Agent header (default: FlowGet/0.1.0)\n"
                 "  --header \"Name: v\"    Extra request header; repeatable\n"
                 "\n"
                 "Resume is on by default: a valid sidecar continues the download.\n"
                 "Rate limits arrive later in M1.\n",
                 static_cast<int>(kProgramName.size()),
                 kProgramName.data());
}

void print_error(std::string_view message) {
    std::fprintf(stderr,
                 "%.*s: error: %.*s\n",
                 static_cast<int>(kProgramName.size()),
                 kProgramName.data(),
                 static_cast<int>(message.size()),
                 message.data());
}

void print_progress(const flowget::DownloadProgress& progress) {
    if (progress.total_bytes > 0) {
        std::fprintf(stderr,
                     "\r%" PRId64 " / %" PRId64 " bytes (%.1f%%)",
                     progress.bytes_written,
                     progress.total_bytes,
                     progress.fraction() * 100.0);
    } else {
        std::fprintf(stderr, "\r%" PRId64 " bytes", progress.bytes_written);
    }
    std::fflush(stderr);
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

// Accepts `Name: value` (the space is optional, like an HTTP header) and stores the trimmed value.
bool add_header(std::map<std::string, std::string>& headers, std::string_view header) {
    const std::size_t colon = header.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view name = trim(header.substr(0, colon));
    if (name.empty()) {
        return false;
    }
    headers[std::string{name}] = std::string{trim(header.substr(colon + 1))};
    return true;
}

bool parse_connections(std::string_view text, int& out) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return false;
    }
    if (value < 1 || value > flowget::kMaxConnections) {
        return false;
    }
    out = value;
    return true;
}

// Runs one download with Ctrl+C wired to the engine's cancel token and reports the result. Returns
// the process exit code: 0 on success, 130 when the user interrupted (shell convention), 1 for any
// other failure.
int download_with_cancel(const flowget::DownloadOptions& options) {
    flowget::HttpClient client;
    flowget::Downloader downloader{client};

    flowget::DownloadSummary summary;
    {
        // The handler lives only for the duration of the transfer: Ctrl+C outside it keeps the
        // platform default, and a second interrupt restores it even here.
        const flowget::cli::ConsoleCancelHandler cancel_handler;
        if (!cancel_handler.installed()) {
            std::fprintf(stderr,
                         "warning: cannot install the Ctrl+C handler; an interrupt will not be "
                         "resumable\n");
        }
        summary = downloader.download(
            options,
            [](const flowget::DownloadProgress& progress) { print_progress(progress); },
            &flowget::cli::ConsoleCancelHandler::token());
    }
    std::fprintf(stderr, "\n");

    if (!summary.ok()) {
        const std::string& detail =
            !summary.message.empty() ? summary.message : summary.http.message;
        print_error(detail.empty() ? flowget::http_error_name(summary.http.error) : detail);
        if (summary.http.error == flowget::HttpError::aborted) {
            std::fprintf(stderr,
                         "note: the partial file and its resume sidecar were kept; rerun the same "
                         "command to continue\n");
            // 128 + SIGINT, the shell convention for "terminated by Ctrl+C".
            return 130;
        }
        return 1;
    }

    if (summary.fallback) {
        std::fprintf(stderr,
                     "note: ranged download failed (%s); saved over a single connection\n",
                     summary.fallback_reason.c_str());
    }
    std::printf("saved %" PRId64 " bytes to %s using %d connection(s)\n",
                summary.bytes_written,
                options.output_path.c_str(),
                summary.connections);
    return 0;
}

int run_get(const std::vector<std::string>& args) {
    std::string url;
    flowget::DownloadOptions options;

    for (std::size_t index = 2; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        const auto next_value = [&](std::string& target) {
            if (index + 1 >= args.size()) {
                print_error("missing value after " + std::string{arg});
                return false;
            }
            target = args[++index];
            return true;
        };

        if (arg == "-o" || arg == "--output") {
            if (!next_value(options.output_path)) {
                return 2;
            }
        } else if (arg == "--connections") {
            std::string value;
            if (!next_value(value)) {
                return 2;
            }
            if (!parse_connections(value, options.connections)) {
                print_error("--connections expects an integer between 1 and 64");
                return 2;
            }
        } else if (arg == "--no-resume") {
            options.resume = false;
        } else if (arg == "--user-agent") {
            if (!next_value(options.user_agent)) {
                return 2;
            }
        } else if (arg == "--header") {
            std::string header;
            if (!next_value(header)) {
                return 2;
            }
            if (!add_header(options.headers, header)) {
                print_error("--header expects \"Name: value\"");
                return 2;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            print_error("unknown option " + std::string{arg});
            return 2;
        } else if (url.empty()) {
            url = arg;
        } else {
            print_error("only one URL is accepted per invocation");
            return 2;
        }
    }

    if (url.empty() || options.output_path.empty()) {
        print_error("get requires a URL and -o <file>");
        print_usage(stderr);
        return 2;
    }
    options.url = url;
    // The engine reports the durable byte count once, before the first data request. A fresh
    // download reports 0 and stays silent; a resumed one tells the user what it is keeping.
    options.on_resume = [](std::int64_t bytes) {
        if (bytes <= 0) {
            return;
        }
        std::fprintf(
            stderr, "Resuming from %.1f MiB\n", static_cast<double>(bytes) / (1024.0 * 1024.0));
        std::fflush(stderr);
    };

    return download_with_cancel(options);
}

void print_optional(std::string_view label, const std::string& value) {
    if (!value.empty()) {
        std::printf("%.*s: %s\n", static_cast<int>(label.size()), label.data(), value.c_str());
    }
}

int run_probe(const std::vector<std::string>& args) {
    std::string url;
    flowget::ProbeOptions options;

    for (std::size_t index = 2; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        const auto next_value = [&](std::string& target) {
            if (index + 1 >= args.size()) {
                print_error("missing value after " + std::string{arg});
                return false;
            }
            target = args[++index];
            return true;
        };

        if (arg == "--user-agent") {
            if (!next_value(options.user_agent)) {
                return 2;
            }
        } else if (arg == "--header") {
            std::string header;
            if (!next_value(header)) {
                return 2;
            }
            if (!add_header(options.headers, header)) {
                print_error("--header expects \"Name: value\"");
                return 2;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            print_error("unknown option " + std::string{arg});
            return 2;
        } else if (url.empty()) {
            url = arg;
        } else {
            print_error("only one URL is accepted per invocation");
            return 2;
        }
    }

    if (url.empty()) {
        print_error("probe requires a URL");
        print_usage(stderr);
        return 2;
    }
    options.url = url;

    flowget::HttpClient client;
    const flowget::ProbeResult result = flowget::probe(client, options);

    if (!result.ok()) {
        print_error(result.message.empty() ? flowget::http_error_name(result.error)
                                           : result.message);
        return 1;
    }

    std::printf("url: %s\n", result.effective_url.c_str());
    std::printf("status: %ld\n", result.status);
    std::printf("range_support: %s\n", flowget::range_support_name(result.range_support));
    if (result.content_length >= 0) {
        std::printf("size: %" PRId64 "\n", result.content_length);
    } else {
        std::printf("size: unknown\n");
    }
    print_optional("accept_ranges", result.accept_ranges);
    print_optional("etag", result.etag);
    print_optional("last_modified", result.last_modified);
    print_optional("content_type", result.content_type);
    print_optional("content_disposition", result.content_disposition);
    std::printf("filename: %s\n", result.suggested_filename.c_str());
    std::printf("head: %s\n", result.head_used ? "used" : "skipped");
    return 0;
}

int handle_flag(std::string_view flag) {
    if (flag == "--version" || flag == "-V") {
        std::printf("%.*s %s (C ABI v%u)\n",
                    static_cast<int>(kProgramName.size()),
                    kProgramName.data(),
                    flowget_version_string(),
                    flowget_api_version());
        std::printf("  %s\n", flowget::dependency_summary().c_str());
        return 0;
    }
    if (flag == "--help" || flag == "-h") {
        print_usage(stdout);
        return 0;
    }
    return -1;
}

int run_cli(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_usage(stderr);
        return 2;
    }

    const std::string_view command = args[1];
    if (command == "get") {
        return run_get(args);
    }
    if (command == "probe") {
        return run_probe(args);
    }

    // Flags are mutually exclusive and terminate the process, so only argv[1] can ever be handled:
    // a loop here returns on its first iteration, which leaves its increment (and anything after
    // it) unreachable (MSVC C4702).
    const int result = handle_flag(command);
    if (result >= 0) {
        return result;
    }
    print_error("unknown argument " + std::string{command});
    print_usage(stderr);
    return 2;
}

} // namespace

#ifdef _WIN32

namespace {

// wmain receives UTF-16 from Windows; the rest of the program (and the core API) speaks UTF-8.
// Converting here keeps Turkish/CJK/emoji paths intact end to end (S3/S4 decision).
bool utf8_from_wide(std::wstring_view text, std::string& out) {
    if (text.empty()) {
        out.clear();
        return true;
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int length = static_cast<int>(text.size());
    const int size = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(size), '\0');
    return ::WideCharToMultiByte(CP_UTF8,
                                 WC_ERR_INVALID_CHARS,
                                 text.data(),
                                 length,
                                 out.data(),
                                 size,
                                 nullptr,
                                 nullptr) == size;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Without this, printf writes the ANSI code page and a UTF-8 path prints as mojibake.
    static_cast<void>(::SetConsoleOutputCP(CP_UTF8));

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int index = 0; index < argc; ++index) {
        std::string argument;
        if (!utf8_from_wide(argv[index] != nullptr ? std::wstring_view{argv[index]}
                                                   : std::wstring_view{},
                            argument)) {
            print_error("the command line is not valid UTF-16");
            return 2;
        }
        args.push_back(std::move(argument));
    }
    return run_cli(args);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    return run_cli(args);
}

#endif

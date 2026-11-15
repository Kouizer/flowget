#include <flowget/build_info.h>
#include <flowget/flowget.h>
#include <flowget/http_client.h>
#include <flowget/probe.h>
#include <flowget/single_stream_download.h>

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kProgramName = "flowget-cli";

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: %.*s <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  get <url> -o <file>   Download one file over a single connection\n"
                 "  probe <url>           Report size, range support and metadata, no download\n"
                 "  --version, -V         Print version information and exit\n"
                 "  --help, -h            Print this help and exit\n"
                 "\n"
                 "Probe options:\n"
                 "  --user-agent <ua>     User-Agent header (default: FlowGet/0.1.0)\n"
                 "  --header \"Name: v\"    Extra request header; repeatable\n"
                 "\n"
                 "Segmented downloads, resume and rate limits arrive later in M1.\n",
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

int run_get(int argc, char** argv) {
    std::string url;
    std::string output;
    std::string user_agent{"FlowGet/0.1.0"};

    for (int index = 2; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto next_value = [&](std::string& target) {
            if (index + 1 >= argc) {
                print_error("missing value after " + std::string{arg});
                return false;
            }
            target = argv[++index];
            return true;
        };

        if (arg == "-o" || arg == "--output") {
            if (!next_value(output)) {
                return 2;
            }
        } else if (arg == "--user-agent") {
            if (!next_value(user_agent)) {
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

    if (url.empty() || output.empty()) {
        print_error("get requires a URL and -o <file>");
        print_usage(stderr);
        return 2;
    }

    flowget::HttpClient client;
    flowget::SingleStreamDownloader downloader{client};

    flowget::DownloadOptions options;
    options.url = url;
    options.output_path = output;
    options.user_agent = user_agent;

    const flowget::DownloadSummary summary = downloader.download(
        options, [](const flowget::DownloadProgress& progress) { print_progress(progress); });
    std::fprintf(stderr, "\n");

    if (!summary.ok()) {
        const std::string& detail =
            !summary.message.empty() ? summary.message : summary.http.message;
        print_error(detail.empty() ? flowget::http_error_name(summary.http.error) : detail);
        return 1;
    }

    std::printf("saved %" PRId64 " bytes to %s\n", summary.bytes_written, output.c_str());
    return 0;
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

void print_optional(std::string_view label, const std::string& value) {
    if (!value.empty()) {
        std::printf("%.*s: %s\n", static_cast<int>(label.size()), label.data(), value.c_str());
    }
}

int run_probe(int argc, char** argv) {
    std::string url;
    flowget::ProbeOptions options;

    for (int index = 2; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto next_value = [&](std::string& target) {
            if (index + 1 >= argc) {
                print_error("missing value after " + std::string{arg});
                return false;
            }
            target = argv[++index];
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
            const std::size_t colon = header.find(':');
            const std::string_view name = trim(std::string_view{header}.substr(0, colon));
            if (colon == std::string::npos || name.empty()) {
                print_error("--header expects \"Name: value\"");
                return 2;
            }
            options.headers[std::string{name}] = std::string{trim(header.substr(colon + 1))};
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "get") {
        return run_get(argc, argv);
    }
    if (command == "probe") {
        return run_probe(argc, argv);
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

#include <flowget/build_info.h>
#include <flowget/flowget.h>
#include <flowget/http_client.h>
#include <flowget/single_stream_download.h>

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
                 "  --version, -V         Print version information and exit\n"
                 "  --help, -h            Print this help and exit\n"
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

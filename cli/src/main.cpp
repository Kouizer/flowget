#include <flowget/build_info.h>
#include <flowget/flowget.h>

#include <cstdio>
#include <string_view>

namespace {

constexpr std::string_view kProgramName = "flowget-cli";

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: %.*s <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  --version, -V   Print version information and exit\n"
                 "  --help, -h      Print this help and exit\n"
                 "\n"
                 "The 'get' command arrives with milestone M1.\n",
                 static_cast<int>(kProgramName.size()),
                 kProgramName.data());
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

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const int result = handle_flag(arg);
        if (result >= 0) {
            return result;
        }
        std::fprintf(stderr,
                     "%.*s: error: unknown argument '%.*s'\n",
                     static_cast<int>(kProgramName.size()),
                     kProgramName.data(),
                     static_cast<int>(arg.size()),
                     arg.data());
        print_usage(stderr);
        return 2;
    }

    print_usage(stderr);
    return 2;
}

// Real-process tests for the resume user surface: the CLI is started as a child process, the
// transfer is interrupted at a checkpoint threshold (never after a fixed sleep), and the same
// command is run again to prove that the bytes on disk were kept.
//
// Why a child process instead of calling Downloader directly: the Windows Ctrl+C path only exists
// in the CLI (SetConsoleCtrlHandler), and a killed process cannot be simulated in-process without
// leaving the output file and sidecar in an OS-defined state.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <flowget/download.h>
#include <flowget/downloader.h>
#include <flowget/http_client.h>
#include <flowget/resume_state.h>
#include <flowget/sha256.h>

#include "support/http_test_server.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#ifndef FLOWGET_CLI_PATH
#error "FLOWGET_CLI_PATH must point at the flowget-cli binary (tests/CMakeLists.txt)"
#endif

namespace {

using flowget::DownloadOptions;
using flowget::DownloadProgress;
using flowget::DownloadSummary;
using flowget::HttpClient;
using flowget::HttpError;
using flowget::test::HttpTestServer;
using flowget::test::Request;
using flowget::test::Response;

constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kPayloadSize = 8 * kMiB;
constexpr int kConnections = 4;
// Per connection, so the whole transfer is ~2 MiB/s: the 2 s checkpoint interval lands at ~50% of
// the file, which leaves a large, unambiguous resumable remainder.
constexpr std::size_t kThrottleBytesPerSecond = 512 * 1024;
constexpr double kInterruptFraction = 0.20;
constexpr const char* kEtag = "\"process-v1\"";
constexpr const char* kLastModified = "Tue, 17 Nov 2026 10:00:00 GMT";

std::string make_payload(std::size_t size, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::string payload(size, '\0');
    std::uint64_t state = seed;
    for (char& byte : payload) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        byte = static_cast<char>(state & 0xFFU);
    }
    return payload;
}

std::filesystem::path make_temp_dir() {
    static std::atomic<int> counter{0};
    const std::filesystem::path directory = std::filesystem::current_path() / "tmp" /
                                            ("process-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string{value.begin(), value.end()};
}

std::filesystem::path from_utf8(const std::string& utf8) {
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

std::filesystem::path cli_path() {
    std::filesystem::path path = from_utf8(std::string{FLOWGET_CLI_PATH});
#ifdef _WIN32
    path.make_preferred();
#endif
    return path;
}

std::vector<std::string> get_args(const std::string& url, const std::string& output_path) {
    return {"get", url, "-o", output_path, "--connections", std::to_string(kConnections)};
}

// The CLI prints "Resuming from %.1f MiB"; reproduce the same formatting so the assertion compares
// the user-visible number, not a rounded approximation.
std::string resume_line(std::int64_t bytes) {
    char buffer[64] = {};
    const int written = std::snprintf(buffer,
                                      sizeof(buffer),
                                      "Resuming from %.1f MiB",
                                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    return written > 0 ? std::string{buffer, static_cast<std::size_t>(written)} : std::string{};
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The sidecar is replaced while it is polled. Windows needs FILE_SHARE_DELETE for the atomic rename
// to succeed next to an open reader; POSIX has no share modes.
std::optional<std::string> read_text_file_shared(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(path.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::string contents;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) !=
               FALSE &&
           read > 0) {
        contents.append(buffer, static_cast<std::size_t>(read));
    }
    static_cast<void>(::CloseHandle(handle));
    return contents;
#else
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
#endif
}

std::optional<flowget::ResumeState> read_sidecar_state(const std::string& output_path) {
    const std::optional<std::string> text =
        read_text_file_shared(from_utf8(flowget::resume_sidecar_path(output_path)));
    if (!text.has_value()) {
        return std::nullopt;
    }
    flowget::ResumeParseResult parsed = flowget::parse_resume_state(*text);
    if (!parsed.ok()) {
        return std::nullopt;
    }
    return std::move(parsed.state);
}

std::string hash_of(const std::string& contents) {
    return flowget::Sha256::hex_digest(contents.data(), contents.size());
}

std::string hash_of_file(const std::string& utf8_path) {
    return hash_of(read_text_file(from_utf8(utf8_path)));
}

std::string hash_of_payload(const std::string& payload) {
    return hash_of(payload);
}

// Starts the CLI and owns its process handle. The output goes to files so a test can read the
// stderr line after the child exits; stdout is captured only for diagnostics.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    // `hidden` prevents a console window for the kill test. The interrupt test needs a shared
    // console, so it passes false.
    bool start(const std::vector<std::string>& arguments,
               const std::filesystem::path& stdout_path,
               const std::filesystem::path& stderr_path,
               bool hidden,
               std::string& error);

    // Non-blocking: returns true when the child has already exited.
    bool try_wait(int& exit_code);

    // Waits up to `timeout`; returns false on timeout (the child keeps running).
    bool wait(std::chrono::milliseconds timeout, int& exit_code);

    // Forceful termination (TerminateProcess / SIGKILL) and reaping.
    bool kill(std::string& error);

    // Ctrl+C equivalent: CTRL_BREAK_EVENT to the child's process group on Windows, SIGINT on POSIX.
    bool send_interrupt(std::string& error);

private:
    void close_handles();

#ifdef _WIN32
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
#else
    pid_t pid_ = -1;
    bool reaped_ = false;
#endif
};

ChildProcess::~ChildProcess() {
#ifdef _WIN32
    if (process_ != nullptr) {
        static_cast<void>(::TerminateProcess(process_, 9));
        static_cast<void>(::WaitForSingleObject(process_, 10 * 1000));
        close_handles();
    }
#else
    if (pid_ > 0 && !reaped_) {
        static_cast<void>(::kill(pid_, SIGKILL));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
    }
#endif
}

void ChildProcess::close_handles() {
#ifdef _WIN32
    if (process_ != nullptr) {
        static_cast<void>(::CloseHandle(process_));
        process_ = nullptr;
    }
#endif
}

#ifdef _WIN32

std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    static_cast<void>(::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), size));
    return wide;
}

// Quotes one argv element for CreateProcessW using the documented CommandLineToArgvW rules.
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    for (std::size_t index = 0; index < argument.size();) {
        std::size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++index;
            ++backslashes;
        }
        if (index == argument.size()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(argument[index]);
        ++index;
    }
    quoted.push_back(L'"');
    return quoted;
}

bool ChildProcess::start(const std::vector<std::string>& arguments,
                         const std::filesystem::path& stdout_path,
                         const std::filesystem::path& stderr_path,
                         bool hidden,
                         std::string& error) {
    const std::filesystem::path executable = cli_path();
    std::wstring command_line = quote_argument(executable.wstring());
    for (const std::string& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(widen_utf8(argument));
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    const auto open_file = [&security](const std::filesystem::path& path, DWORD access) {
        return ::CreateFileW(path.c_str(),
                             access,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    };
    const HANDLE out = open_file(stdout_path, GENERIC_WRITE);
    const HANDLE err = open_file(stderr_path, GENERIC_WRITE);
    const HANDLE in = ::CreateFileW(L"NUL",
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        error = "cannot open the child's standard handles (error " +
                std::to_string(::GetLastError()) + ")";
        if (out != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(out));
        }
        if (err != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(err));
        }
        if (in != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(in));
        }
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in;
    startup.hStdOutput = out;
    startup.hStdError = err;

    PROCESS_INFORMATION information{};
    DWORD flags = CREATE_NEW_PROCESS_GROUP;
    if (hidden) {
        flags |= CREATE_NO_WINDOW;
    }
    const BOOL created = ::CreateProcessW(nullptr,
                                          command_line.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          flags,
                                          nullptr,
                                          nullptr,
                                          &startup,
                                          &information);
    static_cast<void>(::CloseHandle(out));
    static_cast<void>(::CloseHandle(err));
    static_cast<void>(::CloseHandle(in));
    if (created == FALSE) {
        error = "CreateProcessW failed (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    static_cast<void>(::CloseHandle(information.hThread));
    process_ = information.hProcess;
    pid_ = information.dwProcessId;
    return true;
}

bool ChildProcess::try_wait(int& exit_code) {
    if (process_ == nullptr) {
        return false;
    }
    if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(process_, &code) == FALSE) {
        return false;
    }
    exit_code = static_cast<int>(code);
    return true;
}

bool ChildProcess::wait(std::chrono::milliseconds timeout, int& exit_code) {
    if (process_ == nullptr) {
        return false;
    }
    const DWORD milliseconds = static_cast<DWORD>(timeout.count());
    if (::WaitForSingleObject(process_, milliseconds) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(process_, &code) == FALSE) {
        return false;
    }
    exit_code = static_cast<int>(code);
    return true;
}

bool ChildProcess::kill(std::string& error) {
    if (process_ == nullptr) {
        error = "no child process";
        return false;
    }
    if (::TerminateProcess(process_, 9) == FALSE) {
        error = "TerminateProcess failed (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    int ignored = 0;
    static_cast<void>(wait(std::chrono::milliseconds{10 * 1000}, ignored));
    return true;
}

bool ChildProcess::send_interrupt(std::string& error) {
    if (pid_ == 0) {
        error = "no child process";
        return false;
    }
    if (::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_) == FALSE) {
        error = "GenerateConsoleCtrlEvent failed (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    return true;
}

#else

int decode_wait_status(int status) {
    if (WIFEXITED(status) != 0) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status) != 0) {
        // Shell convention: 128 + the terminating signal (SIGINT -> 130).
        return 128 + WTERMSIG(status);
    }
    return -1;
}

bool ChildProcess::start(const std::vector<std::string>& arguments,
                         const std::filesystem::path& stdout_path,
                         const std::filesystem::path& stderr_path,
                         bool /*hidden*/,
                         std::string& error) {
    std::vector<std::string> storage;
    storage.push_back(cli_path().string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        error = "posix_spawn_file_actions_init failed";
        return false;
    }
    const int stdout_result = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int stderr_result = posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    posix_spawnattr_t attributes;
    int attribute_result = posix_spawnattr_init(&attributes);
    if (attribute_result == 0) {
        // Own process group: SIGINT must reach the CLI and nothing else.
        attribute_result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }
    if (attribute_result == 0) {
        attribute_result = posix_spawnattr_setpgroup(&attributes, 0);
    }

    if (stdout_result != 0 || stderr_result != 0 || attribute_result != 0) {
        error = "cannot prepare the child's descriptors or attributes";
        if (attribute_result == 0) {
            static_cast<void>(posix_spawnattr_destroy(&attributes));
        }
        static_cast<void>(posix_spawn_file_actions_destroy(&actions));
        return false;
    }

    const int spawned =
        posix_spawn(&pid_, cli_path().c_str(), &actions, &attributes, argv.data(), environ);
    static_cast<void>(posix_spawnattr_destroy(&attributes));
    static_cast<void>(posix_spawn_file_actions_destroy(&actions));
    if (spawned != 0) {
        error = "posix_spawn failed (" + std::string{std::strerror(spawned)} + ")";
        pid_ = -1;
        return false;
    }
    reaped_ = false;
    return true;
}

bool ChildProcess::try_wait(int& exit_code) {
    if (pid_ <= 0 || reaped_) {
        return false;
    }
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result != pid_) {
        return false;
    }
    reaped_ = true;
    pid_ = -1;
    exit_code = decode_wait_status(status);
    return true;
}

bool ChildProcess::wait(std::chrono::milliseconds timeout, int& exit_code) {
    if (pid_ <= 0 || reaped_) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (try_wait(exit_code)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

bool ChildProcess::kill(std::string& error) {
    if (pid_ <= 0 || reaped_) {
        error = "no child process";
        return false;
    }
    if (::kill(pid_, SIGKILL) != 0) {
        error = "kill(SIGKILL) failed";
        return false;
    }
    int ignored = 0;
    static_cast<void>(wait(std::chrono::milliseconds{10 * 1000}, ignored));
    return true;
}

bool ChildProcess::send_interrupt(std::string& error) {
    if (pid_ <= 0 || reaped_) {
        error = "no child process";
        return false;
    }
    if (::kill(pid_, SIGINT) != 0) {
        error = "kill(SIGINT) failed";
        return false;
    }
    return true;
}

#endif

struct CompletedRun {
    bool exited = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

CompletedRun run_to_completion(const std::vector<std::string>& arguments,
                               const std::filesystem::path& directory,
                               std::string_view name) {
    CompletedRun completed;
    const std::filesystem::path stdout_path = directory / (std::string{name} + ".stdout.txt");
    const std::filesystem::path stderr_path = directory / (std::string{name} + ".stderr.txt");

    ChildProcess process;
    std::string error;
    if (!process.start(arguments, stdout_path, stderr_path, true, error)) {
        ADD_FAILURE() << "cannot start the CLI: " << error;
        return completed;
    }
    completed.exited = process.wait(std::chrono::milliseconds{120 * 1000}, completed.exit_code);
    if (!completed.exited) {
        static_cast<void>(process.kill(error));
        ADD_FAILURE() << "the CLI did not exit within 120 s";
    }
    completed.out = read_text_file(stdout_path);
    completed.err = read_text_file(stderr_path);
    return completed;
}

// Polls the sidecar until it reports at least `threshold` durable bytes. A fixed sleep would make
// the test both slow and timing-dependent; the sidecar is the state the production code acts on.
// Returns -1 (and fills `error`) on timeout or when the child exits first.
std::int64_t wait_for_durable(const std::string& output_path,
                              std::int64_t threshold,
                              ChildProcess& process,
                              std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<flowget::ResumeState> state = read_sidecar_state(output_path);
        if (state.has_value() && state->bytes_done() >= threshold) {
            return state->bytes_done();
        }
        int exit_code = 0;
        if (process.try_wait(exit_code)) {
            error = "the CLI exited with code " + std::to_string(exit_code) +
                    " before the checkpoint threshold";
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    error = "the sidecar did not reach the checkpoint threshold within 60 s";
    return -1;
}

std::int64_t interrupt_threshold() {
    return static_cast<std::int64_t>(static_cast<double>(kPayloadSize) * kInterruptFraction);
}

Response ranged_response(const std::string& payload) {
    Response response;
    response.status = 200;
    response.honor_range = true;
    response.body = payload;
    response.headers["ETag"] = kEtag;
    response.headers["Last-Modified"] = kLastModified;
    response.throttle_bytes_per_second = kThrottleBytesPerSecond;
    return response;
}

// Leaves a valid sidecar and a preallocated partial file behind by cancelling the in-process
// downloader at 40%. Used by the --no-resume test, where the point is the flag, not the kill path.
void create_partial_download(const std::string& url, const std::string& output_path) {
    DownloadOptions options;
    options.url = url;
    options.output_path = output_path;
    options.connections = kConnections;

    std::atomic<bool> cancel{false};
    HttpClient client;
    flowget::Downloader downloader{client};
    const DownloadSummary summary = downloader.download(
        options,
        [&cancel](const DownloadProgress& progress) {
            const auto threshold =
                static_cast<std::int64_t>(static_cast<double>(kPayloadSize) * 0.40);
            if (progress.bytes_written >= threshold) {
                cancel.store(true);
            }
        },
        &cancel);
    EXPECT_EQ(summary.http.error, HttpError::aborted) << summary.message;
}

TEST(ProcessResume, KillAndResumeMatchesTheSourceHash) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) { return ranged_response(payload); });

    const std::filesystem::path directory = make_temp_dir();
    const std::string output_path = to_utf8(directory / "kill.bin");
    const std::string url = server.url("/kill.bin");
    const std::filesystem::path stdout_path = directory / "kill.stdout.txt";
    const std::filesystem::path stderr_path = directory / "kill.stderr.txt";

    ChildProcess process;
    std::string error;
    ASSERT_TRUE(process.start(get_args(url, output_path), stdout_path, stderr_path, true, error))
        << error;

    ASSERT_GE(wait_for_durable(output_path, interrupt_threshold(), process, error), 0) << error;
    ASSERT_TRUE(process.kill(error)) << error;

    // Read after the process is gone: a checkpoint may have landed between the poll and the kill,
    // and the resumed line must match what is actually on disk.
    const std::optional<flowget::ResumeState> state = read_sidecar_state(output_path);
    ASSERT_TRUE(state.has_value()) << "the killed run left no readable sidecar";
    const std::int64_t killed_at = state->bytes_done();
    ASSERT_GT(killed_at, 0);
    ASSERT_LT(killed_at, static_cast<std::int64_t>(payload.size()));

    const CompletedRun resumed = run_to_completion(get_args(url, output_path), directory, "resume");
    ASSERT_TRUE(resumed.exited);
    EXPECT_EQ(resumed.exit_code, 0) << resumed.err;
    EXPECT_EQ(hash_of_file(output_path), hash_of_payload(payload));
    EXPECT_FALSE(std::filesystem::exists(from_utf8(flowget::resume_sidecar_path(output_path))));
    EXPECT_NE(resumed.err.find(resume_line(killed_at)), std::string::npos) << resumed.err;
}

TEST(ProcessResume, InterruptExitsWith130AndResumes) {
#ifdef _WIN32
    // A ConPTY/pseudoconsole has no console window but is still a console, and AllocConsole then
    // fails with ERROR_ACCESS_DENIED. Try only when there is no window, then let the delivery
    // attempt below decide whether the path is testable.
    if (::GetConsoleWindow() == nullptr) {
        static_cast<void>(::AllocConsole());
    }
#endif

    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) { return ranged_response(payload); });

    const std::filesystem::path directory = make_temp_dir();
    const std::string output_path = to_utf8(directory / "interrupt.bin");
    const std::string url = server.url("/interrupt.bin");
    const std::filesystem::path stdout_path = directory / "interrupt.stdout.txt";
    const std::filesystem::path stderr_path = directory / "interrupt.stderr.txt";

    ChildProcess process;
    std::string error;
    ASSERT_TRUE(process.start(get_args(url, output_path), stdout_path, stderr_path, false, error))
        << error;

    ASSERT_GE(wait_for_durable(output_path, interrupt_threshold(), process, error), 0) << error;

#ifdef _WIN32
    if (!process.send_interrupt(error)) {
        static_cast<void>(process.kill(error));
        GTEST_SKIP() << "Windows Ctrl+C could not be delivered automatically (" << error
                     << "); the kill test still proves resume, so this path is manually verified";
    }
#else
    ASSERT_TRUE(process.send_interrupt(error)) << error;
#endif

    int exit_code = -1;
    ASSERT_TRUE(process.wait(std::chrono::milliseconds{120 * 1000}, exit_code))
        << "the CLI ignored the interrupt";
    EXPECT_EQ(exit_code, 130);

    const std::optional<flowget::ResumeState> state = read_sidecar_state(output_path);
    ASSERT_TRUE(state.has_value()) << "the interrupted run left no readable sidecar";
    const std::int64_t interrupted_at = state->bytes_done();

    const CompletedRun resumed = run_to_completion(get_args(url, output_path), directory, "resume");
    ASSERT_TRUE(resumed.exited);
    EXPECT_EQ(resumed.exit_code, 0) << resumed.err;
    EXPECT_EQ(hash_of_file(output_path), hash_of_payload(payload));
    EXPECT_FALSE(std::filesystem::exists(from_utf8(flowget::resume_sidecar_path(output_path))));
    EXPECT_NE(resumed.err.find(resume_line(interrupted_at)), std::string::npos) << resumed.err;
}

TEST(ProcessResume, NoResumeDeletesTheSidecarAndStartsOver) {
    const std::string payload = make_payload(kPayloadSize);
    HttpTestServer server([&payload](const Request&) { return ranged_response(payload); });

    const std::filesystem::path directory = make_temp_dir();
    const std::string output_path = to_utf8(directory / "fresh.bin");
    const std::string url = server.url("/fresh.bin");

    create_partial_download(url, output_path);
    const std::optional<flowget::ResumeState> partial = read_sidecar_state(output_path);
    ASSERT_TRUE(partial.has_value()) << "the in-process run left no usable sidecar";
    ASSERT_GT(partial->bytes_done(), 0);

    std::vector<std::string> arguments = get_args(url, output_path);
    arguments.emplace_back("--no-resume");
    const CompletedRun completed = run_to_completion(arguments, directory, "fresh");
    ASSERT_TRUE(completed.exited);
    EXPECT_EQ(completed.exit_code, 0) << completed.err;
    EXPECT_EQ(completed.err.find("Resuming from"), std::string::npos)
        << "--no-resume must not announce a resume: " << completed.err;
    EXPECT_EQ(hash_of_file(output_path), hash_of_payload(payload));
    EXPECT_FALSE(std::filesystem::exists(from_utf8(flowget::resume_sidecar_path(output_path))));
}

} // namespace

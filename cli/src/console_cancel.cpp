#include "console_cancel.h"

#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

namespace flowget::cli {
namespace {

// A console/signal handler can only reach objects with static storage duration; one state object
// per process is the contract (the CLI runs one download).
struct HandlerState {
    std::atomic<bool> cancel_requested{false};
#ifdef _WIN32
    std::atomic<int> interrupts{0};
#else
    // Only async-signal-safe state: the atomic is lock-free (asserted below) and the sig_atomic_t
    // counter is read and written without locks.
    volatile sig_atomic_t signal_count = 0;
    struct sigaction previous_action{};
#endif
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): written from the handler
HandlerState g_state;

#ifdef _WIN32

BOOL WINAPI console_control_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    // First interrupt: ask the engine to stop at its next poll. Second: return FALSE so Windows
    // runs the default handler and terminates the process, even if the engine is stuck.
    if (g_state.interrupts.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_state.cancel_requested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

#else

// A signal handler may only touch lock-free atomics and sig_atomic_t. The flag is checked at
// compile time so a platform where std::atomic<bool> uses a lock cannot silently build a handler
// that may deadlock.
static_assert(std::atomic<bool>::is_always_lock_free,
              "the Ctrl+C handler stores into std::atomic<bool>; it must not take a lock");

extern "C" void handle_sigint(int /*signal_number*/) {
    if (g_state.signal_count != 0) {
        // Second Ctrl+C: restore the default disposition and re-raise, which terminates the
        // process without writing a checkpoint (documented in tasks/todo.md S5a-2b).
        std::signal(SIGINT, SIG_DFL);
        std::raise(SIGINT);
        return;
    }
    g_state.signal_count = 1;
    g_state.cancel_requested.store(true, std::memory_order_relaxed);
}

#endif

} // namespace

const std::atomic<bool>& ConsoleCancelHandler::token() noexcept {
    return g_state.cancel_requested;
}

ConsoleCancelHandler::ConsoleCancelHandler() {
    g_state.cancel_requested.store(false, std::memory_order_relaxed);
#ifdef _WIN32
    g_state.interrupts.store(0, std::memory_order_relaxed);
    installed_ = ::SetConsoleCtrlHandler(console_control_handler, TRUE) != FALSE;
#else
    g_state.signal_count = 0;
    struct sigaction action{};
    action.sa_handler = handle_sigint;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    installed_ = ::sigaction(SIGINT, &action, &g_state.previous_action) == 0;
#endif
}

ConsoleCancelHandler::~ConsoleCancelHandler() {
    restore();
}

void ConsoleCancelHandler::restore() noexcept {
    if (!installed_) {
        return;
    }
    installed_ = false;
#ifdef _WIN32
    static_cast<void>(::SetConsoleCtrlHandler(console_control_handler, FALSE));
#else
    static_cast<void>(::sigaction(SIGINT, &g_state.previous_action, nullptr));
#endif
}

} // namespace flowget::cli

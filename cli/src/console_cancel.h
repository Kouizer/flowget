#ifndef FLOWGET_CLI_CONSOLE_CANCEL_H
#define FLOWGET_CLI_CONSOLE_CANCEL_H

#include <atomic>

namespace flowget::cli {

// Turns Ctrl+C into a cancel token for the download engine. The first interrupt only flips an
// atomic flag; a second one restores the default behaviour, so a wedged process can always be
// stopped. The handlers never touch files, logs or locks: on Windows they run on a console thread,
// on POSIX in a signal context, where only async-signal-safe operations are allowed.
//
// The engine reads the token between curl_multi_poll rounds (~50 ms), so an interrupt publishes a
// checkpoint instead of losing the bytes written since the previous one.
class ConsoleCancelHandler {
public:
    ConsoleCancelHandler();
    ~ConsoleCancelHandler();

    ConsoleCancelHandler(const ConsoleCancelHandler&) = delete;
    ConsoleCancelHandler& operator=(const ConsoleCancelHandler&) = delete;
    ConsoleCancelHandler(ConsoleCancelHandler&&) = delete;
    ConsoleCancelHandler& operator=(ConsoleCancelHandler&&) = delete;

    // Process-wide: the CLI installs one handler per process, so a second instance would share the
    // same flag. The constructor resets it before installing.
    [[nodiscard]] static const std::atomic<bool>& token() noexcept;

    // False when the platform refused to install the handler; Ctrl+C then keeps its default
    // behaviour (the process dies without a checkpoint).
    [[nodiscard]] bool installed() const noexcept {
        return installed_;
    }

    // Removes the handler early. Idempotent; the destructor calls it too.
    void restore() noexcept;

private:
    bool installed_ = false;
};

} // namespace flowget::cli

#endif /* FLOWGET_CLI_CONSOLE_CANCEL_H */

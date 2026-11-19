#include "utf8_path.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include <windows.h>

namespace flowget::detail {

bool utf8_to_wide(std::string_view utf8, std::wstring& out) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int input_length = static_cast<int>(utf8.size());
    // MB_ERR_INVALID_CHARS: a path that is not valid UTF-8 is a caller bug, not a file name to
    // guess at. Failing here is better than opening a differently named file.
    const int wide_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, nullptr, 0);
    if (wide_length <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(wide_length));
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, out.data(), wide_length);
    return converted == wide_length;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input_length = static_cast<int>(wide.size());
    const int utf8_length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), input_length, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(utf8_length), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), input_length, out.data(), utf8_length, nullptr, nullptr);
    if (converted != utf8_length) {
        return {};
    }
    return out;
}

std::string win32_error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        text = wide_to_utf8(std::wstring_view(buffer, length));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (text.empty()) {
        text = "unknown error";
    }
    return text;
}

} // namespace flowget::detail

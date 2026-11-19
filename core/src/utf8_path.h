#ifndef FLOWGET_SRC_UTF8_PATH_H
#define FLOWGET_SRC_UTF8_PATH_H

#include <string>
#include <string_view>

// Windows-only helpers shared by the storage layer and the sidecar writer. Paths cross the public
// API as UTF-8; everything that calls a wide Win32 entry point converts here, so there is exactly
// one conversion (and one failure mode) in the codebase. POSIX passes UTF-8 bytes straight to the C
// API, which is why this header is empty there.
#ifdef _WIN32

namespace flowget::detail {

// UTF-8 -> UTF-16 with MB_ERR_INVALID_CHARS. Returns false when the input is not valid UTF-8: a
// mangled name would open a different file, which is worse than failing.
bool utf8_to_wide(std::string_view utf8, std::wstring& out);

std::string wide_to_utf8(std::wstring_view wide);

// FormatMessageW text without the trailing CR/LF, in UTF-8.
std::string win32_error_text(unsigned long code);

} // namespace flowget::detail

#endif /* _WIN32 */

#endif /* FLOWGET_SRC_UTF8_PATH_H */

#ifndef FLOWGET_HTTP_FILENAME_H
#define FLOWGET_HTTP_FILENAME_H

#include <string>
#include <string_view>

namespace flowget {

// Best-effort file name from a `Content-Disposition` header value. `filename*` (RFC 5987, UTF-8
// only) wins over `filename`; an absent or unusable parameter yields an empty string and the caller
// falls back to the URL.
//
// The result is always a bare file name: it cannot traverse directories or address a Windows
// stream, because path separators, ':' and the characters Windows rejects are removed, control
// characters are dropped, and "." / ".." are rejected.
std::string filename_from_content_disposition(std::string_view header_value);

// Last segment of the URL's path, percent-decoded and sanitized like the Content-Disposition
// result. Empty when the URL carries no usable segment ("https://example.com/",
// "https://example.com").
std::string filename_from_url(std::string_view url);

} // namespace flowget

#endif /* FLOWGET_HTTP_FILENAME_H */

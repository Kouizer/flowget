#ifndef FLOWGET_HTTP_RANGE_H
#define FLOWGET_HTTP_RANGE_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace flowget {

// Parsed value of a `Content-Range` response header (RFC 9110, section 14.4). The three shapes a
// download manager sees are:
//   `bytes 0-0/12345` -> first=0, last=0,   total=12345   (206, one byte of a 12345-byte file)
//   `bytes 0-999/*`   -> first=0, last=999, total=-1      (206, total length unknown)
//   `bytes */12345`   -> first=-1, last=-1, total=12345   (416, nothing can be satisfied)
// `total` is -1 whenever the header says `*`.
struct ContentRange {
    std::int64_t first = -1;
    std::int64_t last = -1;
    std::int64_t total = -1;

    // False for an unsatisfied range (`bytes */N`): the header is still useful for its total.
    [[nodiscard]] bool has_range() const {
        return first >= 0 && last >= first;
    }
};

// Parses a `Content-Range` field value. Returns std::nullopt for anything that is not a well-formed
// `bytes` range: unknown units, missing separators, non-digits, values that overflow std::int64_t,
// inverted ranges and ranges whose last byte lies outside the announced total. Leading/trailing
// whitespace and a case-insensitive unit ("BYTES") are tolerated.
std::optional<ContentRange> parse_content_range(std::string_view value);

} // namespace flowget

#endif /* FLOWGET_HTTP_RANGE_H */

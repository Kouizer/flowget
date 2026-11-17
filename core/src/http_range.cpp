#include <flowget/http_range.h>

#include <cctype>
#include <limits>
#include <string_view>

namespace flowget {
namespace {

bool is_space(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

// Decimal digits only. Rejects empty input, signs, whitespace and anything that does not fit in a
// non-negative std::int64_t, because a silently truncated byte offset would corrupt a download.
bool parse_offset(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = (value * 10U) + digit;
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

} // namespace

std::optional<ContentRange> parse_content_range(std::string_view value) {
    const std::string_view text = trim(value);

    const std::size_t unit_end = text.find(' ');
    if (unit_end == std::string_view::npos) {
        return std::nullopt;
    }
    if (!equals_ignore_case(text.substr(0, unit_end), "bytes")) {
        return std::nullopt;
    }

    const std::string_view specification = trim(text.substr(unit_end + 1));
    const std::size_t slash = specification.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view position = trim(specification.substr(0, slash));
    const std::string_view size = trim(specification.substr(slash + 1));

    ContentRange range;
    if (size != "*") {
        if (!parse_offset(size, range.total)) {
            return std::nullopt;
        }
    }

    if (position == "*") {
        // `bytes */N` is only meaningful with a known length.
        if (range.total < 0) {
            return std::nullopt;
        }
        return range;
    }

    const std::size_t dash = position.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    if (!parse_offset(trim(position.substr(0, dash)), range.first) ||
        !parse_offset(trim(position.substr(dash + 1)), range.last)) {
        return std::nullopt;
    }
    if (range.last < range.first) {
        return std::nullopt;
    }
    if (range.total >= 0 && range.last >= range.total) {
        return std::nullopt;
    }
    return range;
}

} // namespace flowget

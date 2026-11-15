#include <flowget/http_filename.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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

std::string lowercase(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
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

// Splits a header value on ';' while ignoring separators inside quoted strings.
std::vector<std::string_view> split_parameters(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quoted) {
            if (character == '\\' && index + 1 < value.size()) {
                ++index;
            } else if (character == '"') {
                quoted = false;
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ';') {
            parts.push_back(value.substr(start, index - start));
            start = index + 1;
        }
    }
    parts.push_back(value.substr(start));
    return parts;
}

std::string unquote(std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return std::string{text};
    }
    text.remove_prefix(1);
    text.remove_suffix(1);
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        // Only `\"` and `\\` are quoted-pairs; a bare backslash (common in Windows paths sent by
        // careless servers) is part of the file name and is handled by sanitize().
        if (text[index] == '\\' && index + 1 < text.size() &&
            (text[index + 1] == '"' || text[index + 1] == '\\')) {
            ++index;
        }
        result.push_back(text[index]);
    }
    return result;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::string percent_decode(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const int high = hex_value(text[index + 1]);
            const int low = hex_value(text[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(text[index]);
    }
    return result;
}

// Reduces an arbitrary header/URL-derived string to one safe file name. Keeping only the final path
// component also neutralizes separators introduced by percent-decoding ("%2F../x").
std::string sanitize(std::string_view raw) {
    const std::size_t separator = raw.find_last_of("/\\");
    const std::string_view name =
        separator == std::string_view::npos ? raw : raw.substr(separator + 1);

    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7FU) {
            continue; // control characters would break logs and terminals
        }
        if (character == ':' || character == '<' || character == '>' || character == '|' ||
            character == '"' || character == '?' || character == '*') {
            continue; // invalid in Windows file names, and ':' addresses an NTFS stream
        }
        result.push_back(character);
    }

    // Windows strips trailing dots and spaces, so do it here as well: the reported name must match
    // the file that will actually be created.
    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }
    std::size_t first = 0;
    while (first < result.size() && result[first] == ' ') {
        ++first;
    }
    result.erase(0, first);

    if (result.empty() || result == "." || result == "..") {
        return {};
    }
    return result;
}

// RFC 5987 ext-value: charset'language'percent-encoded. Only UTF-8 is accepted; guessing at other
// charsets would produce mojibake in file names.
std::string decode_extended(std::string_view value) {
    const std::size_t charset_end = value.find('\'');
    if (charset_end == std::string_view::npos) {
        return {};
    }
    const std::size_t language_end = value.find('\'', charset_end + 1);
    if (language_end == std::string_view::npos) {
        return {};
    }
    if (!equals_ignore_case(value.substr(0, charset_end), "utf-8")) {
        return {};
    }
    return percent_decode(value.substr(language_end + 1));
}

} // namespace

std::string filename_from_content_disposition(std::string_view header_value) {
    std::string plain;
    std::string extended;

    for (const std::string_view part : split_parameters(header_value)) {
        const std::size_t equals = part.find('=');
        if (equals == std::string_view::npos) {
            continue; // the disposition type itself ("attachment") has no '='
        }
        const std::string name = lowercase(trim(part.substr(0, equals)));
        const std::string_view value = trim(part.substr(equals + 1));
        if (name == "filename*") {
            if (extended.empty()) {
                extended = decode_extended(unquote(value));
            }
        } else if (name == "filename") {
            if (plain.empty()) {
                plain = unquote(value);
            }
        }
    }

    if (!extended.empty()) {
        std::string name = sanitize(extended);
        if (!name.empty()) {
            return name;
        }
    }
    return sanitize(plain);
}

std::string filename_from_url(std::string_view url) {
    // Query and fragment are not part of the file name; drop them before splitting.
    const std::size_t fragment = url.find('#');
    if (fragment != std::string_view::npos) {
        url = url.substr(0, fragment);
    }
    const std::size_t query = url.find('?');
    if (query != std::string_view::npos) {
        url = url.substr(0, query);
    }

    const std::size_t scheme = url.find("://");
    if (scheme != std::string_view::npos) {
        url.remove_prefix(scheme + 3);
    }
    const std::size_t path_start = url.find('/');
    if (path_start == std::string_view::npos) {
        return {}; // bare host: no file name to derive
    }
    const std::string_view path = url.substr(path_start);
    const std::size_t separator = path.find_last_of('/');
    const std::string_view segment =
        separator == std::string_view::npos ? path : path.substr(separator + 1);
    return sanitize(percent_decode(segment));
}

} // namespace flowget

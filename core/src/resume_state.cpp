#include <flowget/resume_state.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flowget {
namespace {

// ordered_json keeps insertion order, so the file reads in schema order instead of alphabetically.
// It is also the type that makes the output byte-stable for tests.
using Json = nlohmann::ordered_json;

constexpr std::size_t kMaxUrlBytes = std::size_t{8} * 1024;
constexpr std::size_t kMaxValidatorBytes = std::size_t{1024};

ResumeParseResult invalid(std::string message) {
    ResumeParseResult result;
    result.error = std::move(message);
    return result;
}

// Integers only: a float ("done": 1.5) or a string ("done": "1024") is a corrupt sidecar, not a
// value to round or coerce. Unsigned values are range-checked before the signed cast.
bool read_int64(const Json& value, std::int64_t& out) {
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(raw);
        return true;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    out = value.get<std::int64_t>();
    return true;
}

bool read_string(const Json& value, std::size_t limit, std::string& out) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > limit) {
        return false;
    }
    out = text;
    return true;
}

bool has_exact_fields(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size()) {
        return false;
    }
    return std::ranges::all_of(names,
                               [&object](const char* name) { return object.contains(name); });
}

} // namespace

std::int64_t ResumeState::bytes_done() const noexcept {
    std::int64_t total = 0;
    for (const ResumeSegment& segment : segments) {
        total += segment.done;
    }
    return total;
}

std::string serialize_resume_state(const ResumeState& state) {
    Json document;
    document["schema_version"] = state.schema_version;
    document["url"] = state.url;
    document["total_bytes"] = state.total_bytes;
    document["etag"] = state.etag;
    document["last_modified"] = state.last_modified;

    Json segments = Json::array();
    for (const ResumeSegment& segment : state.segments) {
        Json entry;
        entry["start"] = segment.start;
        entry["end"] = segment.end;
        entry["done"] = segment.done;
        segments.push_back(std::move(entry));
    }
    document["segments"] = std::move(segments);

    // replace: a header value that is not valid UTF-8 must not throw out of the serializer. The
    // replacement character makes the sidecar unusable for If-Range, which is the safe outcome.
    return document.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

namespace {

// Scalar fields, in schema order. Returns false and fills `error` on the first problem. Split out
// of parse_resume_state so no single function grows past the readability budget (clang-tidy
// readability-function-cognitive-complexity).
bool parse_header(const Json& document, ResumeState& state, std::string& error) {
    std::int64_t schema_version = 0;
    if (!read_int64(document["schema_version"], schema_version)) {
        error = "schema_version must be an integer";
        return false;
    }
    if (schema_version != kResumeSchemaVersion) {
        error = "unsupported sidecar schema version";
        return false;
    }
    state.schema_version = static_cast<int>(schema_version);

    if (!read_string(document["url"], kMaxUrlBytes, state.url) || state.url.empty()) {
        error = "url must be a non-empty string";
        return false;
    }
    if (!read_int64(document["total_bytes"], state.total_bytes) || state.total_bytes <= 0) {
        error = "total_bytes must be a positive integer";
        return false;
    }
    if (!read_string(document["etag"], kMaxValidatorBytes, state.etag) ||
        !read_string(document["last_modified"], kMaxValidatorBytes, state.last_modified)) {
        error = "validators must be strings";
        return false;
    }
    return true;
}

bool parse_segments(const Json& segments,
                    std::int64_t total_bytes,
                    std::vector<ResumeSegment>& out,
                    std::string& error) {
    if (!segments.is_array() || segments.empty() || segments.size() > kMaxResumeSegments) {
        error = "segments must be a non-empty array of at most 4096 entries";
        return false;
    }
    out.reserve(segments.size());

    std::int64_t next_start = 0;
    for (const Json& entry : segments) {
        if (!has_exact_fields(entry, {"start", "end", "done"})) {
            error = "a segment has missing or unexpected fields";
            return false;
        }
        ResumeSegment segment;
        if (!read_int64(entry["start"], segment.start) || !read_int64(entry["end"], segment.end) ||
            !read_int64(entry["done"], segment.done)) {
            error = "segment fields must be integers";
            return false;
        }
        if (segment.start != next_start) {
            error = "segments must be contiguous and start at zero";
            return false;
        }
        if (segment.end <= segment.start || segment.end > total_bytes) {
            error = "a segment range is empty or outside total_bytes";
            return false;
        }
        if (segment.done < 0 || segment.done > segment.length()) {
            error = "segment done is outside its range";
            return false;
        }
        next_start = segment.end;
        out.push_back(segment);
    }
    if (next_start != total_bytes) {
        error = "segments do not cover total_bytes";
        return false;
    }
    return true;
}

} // namespace

ResumeParseResult parse_resume_state(std::string_view json) {
    if (json.size() > kMaxResumeStateBytes) {
        return invalid("sidecar is larger than 1 MiB");
    }

    // allow_exceptions=false: a malformed file is expected input, not a programming error, and
    // ADR-0002 keeps exceptions from crossing the core boundary.
    const Json document = Json::parse(json.begin(), json.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return invalid("sidecar is not a JSON object");
    }
    if (!has_exact_fields(
            document,
            {"schema_version", "url", "total_bytes", "etag", "last_modified", "segments"})) {
        return invalid("sidecar has missing or unexpected fields");
    }

    ResumeState state;
    std::string error;
    if (!parse_header(document, state, error)) {
        return invalid(std::move(error));
    }
    if (!parse_segments(document["segments"], state.total_bytes, state.segments, error)) {
        return invalid(std::move(error));
    }
    if (!has_resume_validator(state)) {
        return invalid("sidecar has no usable validator (strong ETag or Last-Modified)");
    }

    ResumeParseResult result;
    result.state = std::move(state);
    return result;
}

bool is_strong_etag(std::string_view etag) noexcept {
    if (etag.empty()) {
        return false;
    }
    const bool weak = etag.size() >= 2 && (etag[0] == 'W' || etag[0] == 'w') && etag[1] == '/';
    return !weak;
}

bool has_resume_validator(const ResumeState& state) noexcept {
    return is_strong_etag(state.etag) || !state.last_modified.empty();
}

std::string resume_sidecar_path(const std::string& output_path) {
    return output_path + ".flowget";
}

std::string resume_sidecar_temp_path(const std::string& output_path) {
    return output_path + ".flowget.tmp";
}

} // namespace flowget

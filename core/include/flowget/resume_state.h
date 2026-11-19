#ifndef FLOWGET_RESUME_STATE_H
#define FLOWGET_RESUME_STATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace flowget {

// On-disk format of the `.flowget` sidecar. A reader that does not know the version must reject the
// file instead of guessing: the sidecar decides which bytes are trusted, so silently misreading it
// would corrupt the output (tasks/todo.md S5).
constexpr int kResumeSchemaVersion = 1;

// A sidecar holds at most 64 segments (kMaxConnections) and a few hundred bytes of metadata. A
// larger file is not ours; it is rejected before parsing instead of being loaded into memory.
constexpr std::size_t kMaxResumeStateBytes = std::size_t{1024} * 1024;

// Hard bound on the segment array so a hand-written file cannot request unbounded allocation.
constexpr std::size_t kMaxResumeSegments = 4096;

// One planned range and how much of it is durably on disk.
//
// `start` and `end` are absolute byte offsets (`end` exclusive). `done` counts the bytes already
// written for this segment, so the next request starts at start + done. Counting bytes rather than
// a bitmap keeps a partially written segment resumable; with 8 workers that is the normal case.
struct ResumeSegment {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t done = 0;

    [[nodiscard]] std::int64_t length() const noexcept {
        return end - start;
    }
};

struct ResumeState {
    int schema_version = kResumeSchemaVersion;
    std::string url;
    std::int64_t total_bytes = 0;
    std::string etag;
    std::string last_modified;
    std::vector<ResumeSegment> segments;

    [[nodiscard]] std::int64_t bytes_done() const noexcept;
};

// Canonical JSON: fixed key order, two-space indent, trailing newline. Deterministic output matters
// because the sidecar is rewritten at every checkpoint and tests compare bytes.
std::string serialize_resume_state(const ResumeState& state);

struct ResumeParseResult {
    ResumeState state;
    // Empty on success; a caller-visible reason otherwise ("sidecar has no usable validator", ...).
    std::string error;

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

// Parses and validates a sidecar without throwing (nlohmann::json runs with
// allow_exceptions=false). Rejected: unparseable JSON, unknown/missing fields, unsupported schema
// version, non-integer numbers, empty URL, segments that are not contiguous or do not cover
// total_bytes, done outside [0, length], and files without a usable validator. Callers treat every
// rejection the same way: discard the sidecar and restart from zero.
ResumeParseResult parse_resume_state(std::string_view json);

// A validator must prove byte identity, not just semantic equivalence. `W/...` ETags are weak and
// must not be sent in If-Range (RFC 9110 §13.1.5), so they are not validators on their own; when
// the ETag is missing or weak, Last-Modified is used instead.
bool is_strong_etag(std::string_view etag) noexcept;
bool has_resume_validator(const ResumeState& state) noexcept;

// `<output>.flowget` (the sidecar) and `<output>.flowget.tmp` (the checkpoint in progress).
std::string resume_sidecar_path(const std::string& output_path);
std::string resume_sidecar_temp_path(const std::string& output_path);

} // namespace flowget

#endif /* FLOWGET_RESUME_STATE_H */

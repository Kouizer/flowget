#ifndef FLOWGET_SRC_RESUME_SIDECAR_H
#define FLOWGET_SRC_RESUME_SIDECAR_H

#include <flowget/resume_state.h>

#include <cstdint>
#include <string>
#include <string_view>

// Reading and validating the on-disk sidecar, shared by the segmented engine's entry point
// (`Downloader`) and the single-stream path (S5b). Internal to core: the C ABI never exposes it.
namespace flowget::detail {

struct LoadedSidecar {
    bool exists = false;
    ResumeState state;
    // Non-empty: the file exists but must not be trusted. The caller deletes it and starts fresh.
    std::string discard_reason;
};

// An unreadable file and an invalid one are both reported as `discard_reason`: the caller's only
// safe reaction is the same in both cases.
LoadedSidecar load_sidecar(const std::string& sidecar_path);

bool file_exists(const std::string& utf8_path);

// -1 means "missing or unreadable", which every resume decision treats the same way: start fresh.
std::int64_t file_size_or_unknown(const std::string& utf8_path);

// Best effort: a sidecar that cannot be deleted now is caught by the next run's size check. A
// missing sidecar is success.
void forget_sidecar(const std::string& sidecar_path);

// The sidecar must prove the same representation the response describes. A strong ETag is compared
// byte for byte; otherwise Last-Modified is used. A weak ETag never proves byte identity (RFC 9110
// 13.1.5).
bool validators_match(const ResumeState& state,
                      std::string_view etag,
                      std::string_view last_modified);

// The value for If-Range: a strong ETag when the server has one, otherwise Last-Modified. Empty
// when neither can be used, in which case the caller must not resume (and must not checkpoint).
std::string select_if_range(std::string_view etag, std::string_view last_modified);

} // namespace flowget::detail

#endif /* FLOWGET_SRC_RESUME_SIDECAR_H */

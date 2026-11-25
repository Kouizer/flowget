#include "resume_sidecar.h"

#include "download_util.h"
#include "file_replace.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace flowget::detail {

LoadedSidecar load_sidecar(const std::string& sidecar_path) {
    LoadedSidecar loaded;
    if (!file_exists(sidecar_path)) {
        return loaded;
    }
    loaded.exists = true;

    std::string contents;
    std::string read_error;
    if (!read_file_utf8(sidecar_path, kMaxResumeStateBytes, contents, read_error)) {
        loaded.discard_reason = std::move(read_error);
        return loaded;
    }
    ResumeParseResult parsed = parse_resume_state(contents);
    if (!parsed.ok()) {
        loaded.discard_reason = std::move(parsed.error);
        return loaded;
    }
    loaded.state = std::move(parsed.state);
    return loaded;
}

bool file_exists(const std::string& utf8_path) {
    std::error_code error;
    const bool present = std::filesystem::exists(path_from_utf8(utf8_path), error);
    return !error && present;
}

std::int64_t file_size_or_unknown(const std::string& utf8_path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_from_utf8(utf8_path), error);
    if (error) {
        return -1;
    }
    return static_cast<std::int64_t>(size);
}

void forget_sidecar(const std::string& sidecar_path) {
    std::string ignored;
    static_cast<void>(remove_file(sidecar_path, ignored));
}

bool validators_match(const ResumeState& state,
                      std::string_view etag,
                      std::string_view last_modified) {
    if (is_strong_etag(state.etag)) {
        return is_strong_etag(etag) && state.etag == etag;
    }
    return !last_modified.empty() && state.last_modified == last_modified;
}

std::string select_if_range(std::string_view etag, std::string_view last_modified) {
    if (is_strong_etag(etag)) {
        return std::string{etag};
    }
    return std::string{last_modified};
}

} // namespace flowget::detail

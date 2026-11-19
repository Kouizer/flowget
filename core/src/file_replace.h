#ifndef FLOWGET_SRC_FILE_REPLACE_H
#define FLOWGET_SRC_FILE_REPLACE_H

#include <string>
#include <string_view>

// Crash-consistent file replacement for the resume sidecar (S5). Internal to core: the C ABI never
// exposes it, and tests reach it through the integration target's include path.
namespace flowget::detail {

// Replaces `final_path` with `temp_path` in one step: POSIX rename(2), Windows
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). Both paths are UTF-8. A reader
// therefore sees the old file or the new one, never a partially written file.
bool replace_file(const std::string& temp_path, const std::string& final_path, std::string& error);

// Writes `contents` to `final_path` through `final_path + ".tmp"`: create/truncate the temporary
// file, write, flush (fsync/FlushFileBuffers), close, then replace. On failure the temporary file
// is removed and any previous destination stays untouched.
bool write_file_atomically(const std::string& final_path,
                           std::string_view contents,
                           std::string& error);

// Removes a leftover `<output>.flowget.tmp` from a checkpoint that was interrupted before the
// rename. A missing file is success.
bool remove_stale_sidecar_temp(const std::string& output_path, std::string& error);

} // namespace flowget::detail

#endif /* FLOWGET_SRC_FILE_REPLACE_H */

#ifndef FLOWGET_SRC_FILE_REPLACE_H
#define FLOWGET_SRC_FILE_REPLACE_H

#include <cstddef>
#include <string>
#include <string_view>

// Crash-consistent file replacement for the resume sidecar (S5). Internal to core: the C ABI never
// exposes it, and tests reach it through the integration target's include path.
namespace flowget::detail {

// Reads a whole UTF-8 path through IStorage. On Windows that matters twice:
//   * the path is converted UTF-8 -> UTF-16 instead of going through the ANSI code page;
//   * CreateFileW uses FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, so a reader does not
//     block the checkpoint's atomic rename with ERROR_SHARING_VIOLATION. A plain std::ifstream
//     does not share delete and would make a sidecar poller break the engine's own checkpoint.
// Files larger than `limit` are rejected before allocating, so a hand-written huge file cannot
// exhaust memory while deciding whether to resume.
bool read_file_utf8(const std::string& utf8_path,
                    std::size_t limit,
                    std::string& contents,
                    std::string& error);

// Replaces `final_path` with `temp_path` in one step: POSIX rename(2), Windows ReplaceFileW when
// the destination exists (MoveFileExW otherwise). Both paths are UTF-8. A reader therefore sees the
// old file or the new one, never a partially written file. On Windows ReplaceFileW is required
// because MoveFileExW refuses to replace a destination that any process still has open, even with
// FILE_SHARE_DELETE (tasks/lessons.md).
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

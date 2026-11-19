#ifndef FLOWGET_SRC_RESUME_CHECKPOINT_H
#define FLOWGET_SRC_RESUME_CHECKPOINT_H

#include <flowget/resume_state.h>

#include <string>

namespace flowget {
class IStorage;
}

// Crash-consistent resume checkpoint (S5). Internal to core: the C ABI never exposes it, and tests
// reach it through the integration/unit targets' include path.
namespace flowget::detail {

enum class CheckpointResult {
    // The data file and the sidecar are both durable.
    published = 0,
    // fsync/FlushFileBuffers on the data file failed. The sidecar was deliberately not touched:
    // a sidecar that claims bytes which may still be in the page cache would survive a crash
    // without the data it points at (tasks/todo.md S5).
    data_flush_failed,
    // The data is durable but the sidecar could not be replaced. The previous sidecar stays valid;
    // the engine retries at the next trigger.
    sidecar_failed,
};

// Makes every byte written so far durable, then atomically replaces the sidecar:
//   1. data.flush() (fsync / FlushFileBuffers)
//   2. serialize + write `<sidecar>.tmp` through IStorage, flush it
//   3. rename over the sidecar (POSIX rename / MoveFileExW REPLACE_EXISTING|WRITE_THROUGH)
//
// A reader therefore sees the old sidecar or the new one, never a partial file, and never a
// sidecar that points at data which was not flushed. `error` is filled for both failure kinds.
CheckpointResult publish_resume_checkpoint(IStorage& data,
                                           const std::string& sidecar_path,
                                           const ResumeState& state,
                                           std::string& error);

} // namespace flowget::detail

#endif /* FLOWGET_SRC_RESUME_CHECKPOINT_H */

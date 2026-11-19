#include "resume_checkpoint.h"

#include <flowget/storage.h>

#include "file_replace.h"

#include <utility>

namespace flowget::detail {

CheckpointResult publish_resume_checkpoint(IStorage& data,
                                           const std::string& sidecar_path,
                                           const ResumeState& state,
                                           std::string& error) {
    if (sidecar_path.empty()) {
        error = "cannot publish a checkpoint: the sidecar path is empty";
        return CheckpointResult::sidecar_failed;
    }

    // Order matters: the sidecar may only claim bytes that already reached the device. If this
    // flush fails the previous sidecar must stay untouched (tasks/todo.md S5 decision 7).
    const StorageResult flushed = data.flush();
    if (!flushed.ok()) {
        error = "cannot flush the downloaded data before a checkpoint: " +
                (flushed.message.empty() ? storage_error_name(flushed.error) : flushed.message);
        return CheckpointResult::data_flush_failed;
    }

    std::string write_error;
    if (!write_file_atomically(sidecar_path, serialize_resume_state(state), write_error)) {
        error = std::move(write_error);
        return CheckpointResult::sidecar_failed;
    }
    return CheckpointResult::published;
}

} // namespace flowget::detail

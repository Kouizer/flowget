#include <flowget/storage.h>

namespace flowget {

const char* storage_error_name(StorageError error) noexcept {
    switch (error) {
    case StorageError::none:
        return "none";
    case StorageError::invalid_argument:
        return "invalid_argument";
    case StorageError::not_open:
        return "not_open";
    case StorageError::already_open:
        return "already_open";
    case StorageError::open:
        return "open";
    case StorageError::read:
        return "read";
    case StorageError::write:
        return "write";
    case StorageError::preallocate:
        return "preallocate";
    case StorageError::preallocate_unsupported:
        return "preallocate_unsupported";
    case StorageError::flush:
        return "flush";
    case StorageError::close:
        return "close";
    case StorageError::query_size:
        return "query_size";
    }
    return "unknown";
}

bool storage_error_is_fatal(StorageError error) noexcept {
    // `preallocate_unsupported` only means the filesystem cannot reserve clusters up front; the
    // file was still extended, so the download continues and a later write may still fail with
    // ENOSPC.
    return error != StorageError::none && error != StorageError::preallocate_unsupported;
}

} // namespace flowget

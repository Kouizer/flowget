#ifndef FLOWGET_SRC_STORAGE_INTERNAL_H
#define FLOWGET_SRC_STORAGE_INTERNAL_H

#include <flowget/storage.h>

#include <string>
#include <utility>

// Helpers shared by the two platform implementations. Not installed with the public headers.
namespace flowget::detail {

inline StorageResult
make_storage_error(StorageError error, int platform_code, std::string message) {
    StorageResult result;
    result.error = error;
    result.platform_code = platform_code;
    result.message = std::move(message);
    return result;
}

inline StorageResult not_open_error() {
    return make_storage_error(StorageError::not_open, 0, "storage is not open");
}

inline StorageResult invalid_argument_error(std::string message) {
    return make_storage_error(StorageError::invalid_argument, 0, std::move(message));
}

} // namespace flowget::detail

#endif /* FLOWGET_SRC_STORAGE_INTERNAL_H */

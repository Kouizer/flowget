#ifndef FLOWGET_FLOWGET_H
#define FLOWGET_FLOWGET_H

#include <stdint.h>

/*
 * FlowGet C ABI — the only supported boundary between the engine and its clients
 * (flowget-cli, dart:ffi bindings, the Android service).
 *
 * Contract rules (see docs/adr/0002-c-abi-contract.md):
 *   - Strings are UTF-8, NUL terminated, owned by the callee unless stated otherwise.
 *   - Returned pointers stay valid until the owning handle is destroyed or the function says
 * otherwise.
 *   - No C++ exceptions cross this boundary. Every fallible function returns flowget_status.
 *   - Functions are thread-safe unless documented otherwise; callbacks are invoked from a single
 *     engine thread, never from the caller's thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWGET_API_VERSION 1u

#if defined(_WIN32) && defined(FLOWGET_SHARED)
#if defined(FLOWGET_BUILDING)
#define FLOWGET_API __declspec(dllexport)
#else
#define FLOWGET_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(FLOWGET_SHARED)
#define FLOWGET_API __attribute__((visibility("default")))
#else
#define FLOWGET_API
#endif

typedef enum flowget_status {
    FLOWGET_STATUS_OK = 0,
    FLOWGET_STATUS_INVALID_ARGUMENT = 1,
    FLOWGET_STATUS_OUT_OF_MEMORY = 2,
    FLOWGET_STATUS_NOT_FOUND = 3,
    FLOWGET_STATUS_BUSY = 4,
    FLOWGET_STATUS_UNSUPPORTED = 5,
    FLOWGET_STATUS_CANCELLED = 6,
    FLOWGET_STATUS_IO = 7,
    FLOWGET_STATUS_NETWORK = 8,
    FLOWGET_STATUS_RANGE_NOT_SUPPORTED = 9,
    FLOWGET_STATUS_CHECKSUM_MISMATCH = 10,
    FLOWGET_STATUS_INTERNAL = 11,
    FLOWGET_STATUS_UNKNOWN = 12
} flowget_status;

/* Version of the ABI itself. Clients must check this before calling anything else. */
FLOWGET_API uint32_t flowget_api_version(void);

/* Semantic version of the engine, e.g. "0.1.0". */
FLOWGET_API const char* flowget_version_string(void);

/* Stable, human-readable name for a status code ("ok", "network", ...). Never NULL. */
FLOWGET_API const char* flowget_status_name(flowget_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLOWGET_FLOWGET_H */

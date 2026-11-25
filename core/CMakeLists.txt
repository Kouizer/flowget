add_library(
    flowget_core STATIC
    src/version.cpp
    src/api.cpp
    src/build_info.cpp
    src/http_client.cpp
    src/curl_common.cpp
    src/curl_transport.cpp
    src/http_range.cpp
    src/http_filename.cpp
    src/probe.cpp
    src/segment_planner.cpp
    src/sha256.cpp
    src/download.cpp
    src/single_stream_download.cpp
    src/segmented_download.cpp
    src/downloader.cpp
    src/resume_state.cpp
    src/resume_checkpoint.cpp
    src/resume_sidecar.cpp
    src/file_replace.cpp
    src/storage.cpp)

# One interface, one implementation per platform. Keeping the POSIX code out of the Windows build (and
# vice versa) means a wrong include cannot hide behind an #if that nobody compiles.
if(WIN32)
    target_sources(flowget_core PRIVATE src/storage_win32.cpp src/utf8_path.cpp)
else()
    target_sources(flowget_core PRIVATE src/storage_posix.cpp)
    # 32-bit Android uses a 32-bit off_t unless this is defined before any system header; without it
    # a >2 GiB download would silently wrap. storage_posix.cpp static_asserts the result.
    target_compile_definitions(flowget_core PRIVATE _FILE_OFFSET_BITS=64)
endif()
add_library(FlowGet::core ALIAS flowget_core)

target_include_directories(
    flowget_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>)

# curl.h pulls in windows.h, which defines min/max as macros and breaks std::min and
# std::numeric_limits<T>::max() (seen in curl_transport.cpp). Defining NOMINMAX once for the target is
# the only reliable guard: a per-file #define is forgotten as soon as a new file includes curl.h.
# PUBLIC so consumers that also touch sockets/windows.h inherit it. See tasks/lessons.md.
if(WIN32)
    target_compile_definitions(flowget_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# The public C ABI header lives next to the C++ headers so that the CLI, tests, and later the
# dart:ffi bindings consume exactly the same declaration.
set_target_properties(flowget_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/generated/include/flowget/version_generated.h"
    @ONLY)

# Third-party dependencies come from vcpkg manifest mode (vcpkg.json). The CURL backend differs per
# platform on purpose: schannel on Windows uses the system trust store, OpenSSL elsewhere. OpenSSL is
# linked directly for EVP (SHA-256/MD5 verification), SQLite for the queue database (M2), spdlog for
# logging (ADR-0001, ADR-0003). nlohmann/json writes the resume sidecar (S5); it is header-only, so it
# adds compile time only where it is included (resume_state.cpp).
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# PRIVATE is enough: CMake propagates these to static-library consumers as $<LINK_ONLY:...>, so the
# CLI and tests link them without including third-party headers in the public interface.
target_link_libraries(
    flowget_core
    PRIVATE CURL::libcurl
            OpenSSL::Crypto
            SQLite3::SQLite3
            spdlog::spdlog
            nlohmann_json::nlohmann_json)

flowget_set_warnings(flowget_core)

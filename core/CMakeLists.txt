add_library(
    flowget_core STATIC
    src/version.cpp
    src/api.cpp
    src/build_info.cpp
    src/http_client.cpp
    src/curl_transport.cpp
    src/http_range.cpp
    src/http_filename.cpp
    src/probe.cpp
    src/single_stream_download.cpp)
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
# logging (ADR-0001, ADR-0003).
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# PRIVATE is enough: CMake propagates these to static-library consumers as $<LINK_ONLY:...>, so the
# CLI and tests link them without including third-party headers in the public interface.
target_link_libraries(
    flowget_core
    PRIVATE CURL::libcurl
            OpenSSL::Crypto
            SQLite3::SQLite3
            spdlog::spdlog)

flowget_set_warnings(flowget_core)

cmake_minimum_required(VERSION 3.25)

# Dependency management.
#
# The manifest is authoritative: vcpkg.json pins a registry baseline commit, so every checkout
# builds with the same library versions (ADR-0004 keeps GPL/AGPL out of that manifest).
# The toolchain must be active before project() so find_package() resolves to vcpkg ports.

# Root selection: FLOWGET_VCPKG_ROOT wins over VCPKG_ROOT. The override exists because the
# Visual-Studio-bundled cmake.exe rewrites VCPKG_ROOT in the environment its configure step sees
# (verified with `cmake -E environment`), which would silently build against VS's own vcpkg copy
# instead of the one the developer installed. See tasks/lessons.md, 2026-11-14.
if(DEFINED ENV{FLOWGET_VCPKG_ROOT})
    set(FLOWGET_VCPKG_ROOT "$ENV{FLOWGET_VCPKG_ROOT}")
elseif(DEFINED ENV{VCPKG_ROOT})
    set(FLOWGET_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT FLOWGET_VCPKG_ROOT)
        message(
            FATAL_ERROR
                "No vcpkg root found. Install vcpkg (https://github.com/microsoft/vcpkg) and set "
                "VCPKG_ROOT (or FLOWGET_VCPKG_ROOT when using Visual Studio's bundled CMake); see "
                "README.md, section 'Build (Windows)'.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE
        "${FLOWGET_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg manifest-mode toolchain")
endif()

# vcpkg writes absolute paths into the generated `<port>Config.cmake` files; on Windows those are
# parsed in the ANSI code page, so a checkout below a non-ASCII directory ("Varsayılan Proje")
# truncates them and port builds fail with "Imported target includes non-existent path". Keep the
# default layout for ASCII checkouts (CI and most developers) and relocate only when necessary.
if(NOT VCPKG_INSTALLED_DIR AND NOT CMAKE_SOURCE_DIR MATCHES "^[ -~]+$")
    if(NOT DEFINED ENV{LOCALAPPDATA})
        message(FATAL_ERROR
                "Non-ASCII source path and LOCALAPPDATA is unset; point VCPKG_INSTALLED_DIR at an "
                "ASCII directory.")
    endif()
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/FlowGet/vcpkg-installed" _flowget_vcpkg_installed)
    set(VCPKG_INSTALLED_DIR
        "${_flowget_vcpkg_installed}"
        CACHE PATH "vcpkg install tree (relocated: non-ASCII checkout path)")
    message(STATUS "vcpkg install tree: ${VCPKG_INSTALLED_DIR} (non-ASCII source path)")
endif()

project(
    FlowGet
    VERSION 0.1.0
    DESCRIPTION "Fast segmented download manager for Windows and Android"
    HOMEPAGE_URL "https://github.com/flowget/flowget"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        RelWithDebInfo
        CACHE STRING "Build type" FORCE)
endif()

option(FLOWGET_BUILD_CLI "Build the flowget-cli reference client" ON)
option(FLOWGET_BUILD_TESTS "Build unit and integration tests" ON)
option(FLOWGET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(core)

if(FLOWGET_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(FLOWGET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "FlowGet ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | compilers: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

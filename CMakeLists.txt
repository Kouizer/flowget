cmake_minimum_required(VERSION 3.25)

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

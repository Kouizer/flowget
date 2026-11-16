include(FetchContent)

# Pinned release: build machines are offline-capable after the first configure, and CI gets a
# reproducible dependency. Bump deliberately, never automatically.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)

set(gtest_force_shared_crt
    ON
    CACHE BOOL "" FORCE)
set(INSTALL_GTEST
    OFF
    CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)

add_executable(flowget_unit_tests
               unit/version_test.cpp
               unit/build_info_test.cpp
               unit/probe_parsing_test.cpp
               unit/storage_test.cpp
               unit/segment_planner_test.cpp
               unit/sha256_test.cpp
)
target_link_libraries(flowget_unit_tests PRIVATE FlowGet::core GTest::gtest_main)

flowget_set_warnings(flowget_unit_tests)

# Integration tests run against the in-process test HTTP server (tests/support), so they need no
# network access and no external process. The port is chosen by the OS on loopback.
add_executable(flowget_integration_tests
               integration/single_stream_download_test.cpp
               integration/downloader_test.cpp
               integration/probe_test.cpp
               integration/storage_io_test.cpp
               support/http_test_server.cpp)
target_include_directories(flowget_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flowget_integration_tests PRIVATE FlowGet::core GTest::gtest_main
                                                      Threads::Threads)
if(WIN32)
    target_link_libraries(flowget_integration_tests PRIVATE ws2_32)
endif()

flowget_set_warnings(flowget_integration_tests)

# `gtest_discover_tests` runs the binary with `--gtest_output=json:<path>` after linking. GoogleTest
# opens that path with the ANSI code page, so a checkout below a non-ASCII directory (e.g.
# "Varsayılan Proje") fails the *build*. Register each suite as one CTest entry instead; GoogleTest
# still reports per-test results, and `--gtest_filter` is available for focused runs.
# See tasks/lessons.md (2026-11-14).
add_test(NAME flowget_unit_tests COMMAND flowget_unit_tests)
set_tests_properties(flowget_unit_tests PROPERTIES TIMEOUT 120)

add_test(NAME flowget_integration_tests COMMAND flowget_integration_tests)
set_tests_properties(flowget_integration_tests
                     PROPERTIES TIMEOUT 300 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

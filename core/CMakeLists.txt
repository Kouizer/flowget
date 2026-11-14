add_library(flowget_core STATIC src/version.cpp src/api.cpp)
add_library(FlowGet::core ALIAS flowget_core)

target_include_directories(
    flowget_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>)

# The public C ABI header lives next to the C++ headers so that the CLI, tests, and later the
# dart:ffi bindings consume exactly the same declaration.
set_target_properties(flowget_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/generated/include/flowget/version_generated.h"
    @ONLY)

flowget_set_warnings(flowget_core)

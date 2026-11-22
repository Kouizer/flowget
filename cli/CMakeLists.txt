add_executable(flowget-cli src/main.cpp src/console_cancel.cpp)
add_executable(FlowGet::cli ALIAS flowget-cli)

target_link_libraries(flowget-cli PRIVATE FlowGet::core)

flowget_set_warnings(flowget-cli)

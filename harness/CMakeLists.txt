add_executable(harness harness.cpp)
target_link_libraries(harness PRIVATE winmm shared_ipc)
target_link_options(harness PRIVATE -static)
add_dependencies(harness speedhack)
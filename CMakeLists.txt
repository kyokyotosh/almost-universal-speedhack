cmake_minimum_required(VERSION 3.16)
project(almost-universal-speedhack VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Everything lands in one folder so the GUI finds speedhack.dll beside itself
# (and the x86 artifacts will sit here too).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master)
FetchContent_MakeAvailable(minhook)

# The IPC contract as a header-only interface library — link it to get the path.
add_library(shared_ipc INTERFACE)
target_include_directories(shared_ipc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared)

add_subdirectory(dll)
add_subdirectory(harness)
add_subdirectory(gui)
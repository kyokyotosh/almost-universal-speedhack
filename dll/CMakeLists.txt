add_library(speedhack SHARED speedhack.cpp)
target_link_libraries(speedhack PRIVATE minhook shared_ipc)
set_target_properties(speedhack PROPERTIES PREFIX "")   # libspeedhack.dll -> speedhack.dll
target_link_options(speedhack PRIVATE -static)
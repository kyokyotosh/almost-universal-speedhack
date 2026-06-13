find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(almost-universal-speedhack
    main.cpp
    mainwindow.cpp mainwindow.h mainwindow.ui
    injector.cpp injector.h)

target_link_libraries(almost-universal-speedhack PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    advapi32 shell32
    shared_ipc)

set_target_properties(almost-universal-speedhack PROPERTIES WIN32_EXECUTABLE TRUE)
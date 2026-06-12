cmake_minimum_required(VERSION 3.16)

project(almost-universal-speedhack VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        injector.cpp
        injector.h
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(almost-universal-speedhack
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET almost-universal-speedhack APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(almost-universal-speedhack SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(almost-universal-speedhack
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(almost-universal-speedhack PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.almost-universal-speedhack)
endif()
set_target_properties(almost-universal-speedhack PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS almost-universal-speedhack
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(almost-universal-speedhack)
endif()

# ---------------------------------------------------------------------------
# Speedhack payload DLL + test harness  (Windows-only, no Qt)
# ---------------------------------------------------------------------------
if(WIN32)
    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        master
    )
    FetchContent_MakeAvailable(minhook)

    add_library(speedhack SHARED speedhack.cpp)
    target_link_libraries(speedhack PRIVATE minhook)
    set_target_properties(speedhack PROPERTIES PREFIX "")        # libspeedhack.dll -> speedhack.dll
    target_link_options(speedhack PRIVATE -static)    

    add_executable(harness harness.cpp)
    target_link_options(harness PRIVATE -static)
    target_link_libraries(harness PRIVATE winmm)

    # Guarantee the DLL is built before the harness, and lands beside it.
    add_dependencies(harness speedhack)
    add_custom_command(TARGET harness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:speedhack>
            $<TARGET_FILE_DIR:harness>
        COMMENT "Copying speedhack.dll next to harness.exe")
endif()
cmake_minimum_required(VERSION 3.21)
# Product name: Motiva (built as Motiva.exe below). The source tree folder
# (VideoWallpaper/) and internal C++ class names are intentionally left
# as-is - only the build target/executable and product-facing identity
# were renamed. See CLAUDE.md's "Planned work" section.
project(Motiva LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT WIN32)
    message(FATAL_ERROR "Motiva only builds for Windows (uses Win32 desktop APIs).")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Concurrent)

qt_standard_project_setup()

# Implementation files (scripts/src/) and their corresponding headers
# (scripts/include/) are listed separately from the executable entry point
# (scripts/main/main.cpp) - see CLAUDE.md's source-layout notes. Headers
# are listed alongside their .cpp here (as before the move) purely so they
# show up in IDE project trees; they compile via #include, not directly.
set(IMPLEMENTATION_SOURCES
    scripts/src/MainWindow.cpp
    scripts/include/MainWindow.h
    scripts/src/SettingsDialog.cpp
    scripts/include/SettingsDialog.h
    scripts/src/VideoPlayer.cpp
    scripts/include/VideoPlayer.h
    scripts/src/WallpaperWindow.cpp
    scripts/include/WallpaperWindow.h
    scripts/src/D3DWallpaperRenderer.cpp
    scripts/include/D3DWallpaperRenderer.h
    scripts/src/WallpaperManager.cpp
    scripts/include/WallpaperManager.h
    scripts/src/WindowsDesktopWallpaper.cpp
    scripts/include/WindowsDesktopWallpaper.h
    scripts/src/SettingsManager.cpp
    scripts/include/SettingsManager.h
    scripts/src/RecoveryState.cpp
    scripts/include/RecoveryState.h
    scripts/include/StartupDiagnostics.h
    scripts/src/InstanceIpc.cpp
    scripts/include/InstanceIpc.h
)

set(ENTRY_POINT_SOURCES
    scripts/main/main.cpp
)

qt_add_executable(Motiva WIN32 ${ENTRY_POINT_SOURCES} ${IMPLEMENTATION_SOURCES} resources/app.rc resources/app.qrc)

target_link_libraries(Motiva PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Concurrent
    Dwmapi
    Shell32
    User32
    Gdi32
    Advapi32
    d3d11
    dxgi
    dcomp
    d3dcompiler
)

target_include_directories(Motiva PRIVATE scripts/include)

set_target_properties(Motiva PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Per-monitor DPI awareness declared via manifest so physical-pixel window
# placement on multiple monitors with different scale factors is correct.
if(MSVC)
    target_link_options(Motiva PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()

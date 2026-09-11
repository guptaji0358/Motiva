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

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/VideoPlayer.cpp
    src/VideoPlayer.h
    src/WallpaperWindow.cpp
    src/WallpaperWindow.h
    src/D3DWallpaperRenderer.cpp
    src/D3DWallpaperRenderer.h
    src/WallpaperManager.cpp
    src/WallpaperManager.h
    src/WindowsDesktopWallpaper.cpp
    src/WindowsDesktopWallpaper.h
    src/SettingsManager.cpp
    src/SettingsManager.h
    src/RecoveryState.cpp
    src/RecoveryState.h
    src/StartupDiagnostics.h
    src/InstanceIpc.cpp
    src/InstanceIpc.h
)

qt_add_executable(Motiva WIN32 ${SOURCES} resources/app.rc)

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

target_include_directories(Motiva PRIVATE src)

set_target_properties(Motiva PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Per-monitor DPI awareness declared via manifest so physical-pixel window
# placement on multiple monitors with different scale factors is correct.
if(MSVC)
    target_link_options(Motiva PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()

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

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Concurrent Svg)

qt_standard_project_setup()

# Implementation files (scripts/src/) and their corresponding headers
# (scripts/include/) are listed separately from the executable entry point
# (scripts/main/main.cpp) - see CLAUDE.md's source-layout notes. Headers
# are listed alongside their .cpp here (as before the move) purely so they
# show up in IDE project trees; they compile via #include, not directly.
set(IMPLEMENTATION_SOURCES
    scripts/src/Theme.cpp
    scripts/include/Theme.h
    scripts/src/MainWindow.cpp
    scripts/include/MainWindow.h
    scripts/src/DropZoneWidget.cpp
    scripts/include/DropZoneWidget.h
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
    Qt6::Svg
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

# --- Deployment-root launcher (see the "Organize Final Deployment
# Output" task / deploy/Deploy.ps1) ---
# A tiny, Qt-free native executable - EXCLUDE_FROM_ALL so a plain
# `cmake --build build` (the normal dev inner loop) never builds it and
# the dev workflow is completely unaffected; it's only built as part of
# the opt-in `deploy` target below, or by name
# (`cmake --build build --target MotivaLauncher`). Deliberately kept as
# "MotivaLauncher.exe" inside the build tree itself (never named
# "Motiva.exe" there) so it can never collide with the real Motiva.exe
# output in the same directory - deploy/Deploy.ps1 renames it to
# "Motiva.exe" only once it copies it out to the deployment root.
add_executable(MotivaLauncher WIN32 EXCLUDE_FROM_ALL scripts/main/launcher.cpp)
target_link_libraries(MotivaLauncher PRIVATE Shell32 User32)
if(MINGW)
    # MinGW's default CRT startup expects an ANSI WinMain unless told
    # otherwise - -municode switches it to the wWinMainCRTStartup entry
    # point that matches launcher.cpp's wWinMain(...).
    target_compile_options(MotivaLauncher PRIVATE -municode)
    target_link_options(MotivaLauncher PRIVATE -municode)
endif()

# `cmake --build build --target deploy` assembles the clean deployment
# directory described in that task: builds both executables, then hands
# off to deploy/Deploy.ps1 to stage windeployqt's output and reorganize
# it into <repo>/deployment/{Motiva.exe, resources/{bin,Qt,plugins}} -
# see that script for the full reasoning (also runnable directly without
# CMake at all, e.g. from a scripted CI/release pipeline).
get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
add_custom_target(deploy
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/deploy/Deploy.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
            -QtBinDir "${_qt_bin_dir}"
            -OutDir "${CMAKE_SOURCE_DIR}/deployment"
    DEPENDS Motiva MotivaLauncher
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Assembling clean deployment directory in deployment/ (see deploy/Deploy.ps1)"
    VERBATIM
)

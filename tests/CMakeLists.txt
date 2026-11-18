cmake_minimum_required(VERSION 3.21)
project(MotivaTests LANGUAGES CXX)

# Standalone, opt-in unit tests (not part of the Motiva build). They compile
# only the Qt-light Binary Rain model and engine, so they need Qt Core/Gui/Test
# and no Multimedia, D3D or Win32 desktop code.
#   cmake -S tests -B <dir> -G Ninja -DCMAKE_PREFIX_PATH=<Qt kit>
#   cmake --build <dir> && <dir>/BinaryRainTests
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Test)

qt_add_executable(BinaryRainTests
    BinaryRainTests.cpp
    ../scripts/src/BinaryRainConfig.cpp
    ../scripts/src/BinaryRainEngine.cpp)
target_include_directories(BinaryRainTests PRIVATE ../scripts/include)
target_link_libraries(BinaryRainTests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)

qt_add_executable(DesktopScalingTests DesktopScalingTests.cpp)
target_include_directories(DesktopScalingTests PRIVATE ../scripts/include)
target_link_libraries(DesktopScalingTests PRIVATE Qt6::Core Qt6::Test)

# VideoPlayer lifecycle test: needs Qt Multimedia and the D3D libs (VideoPlayer
# links D3DWallpaperRenderer::tagAsStill). Skipped when Multimedia is absent.
find_package(Qt6 QUIET COMPONENTS Widgets Multimedia Concurrent)
if(TARGET Qt6::Multimedia AND TARGET Qt6::Widgets AND TARGET Qt6::Concurrent)
    qt_add_executable(VideoPlayerRainTests
        VideoPlayerRainTests.cpp
        ../scripts/src/BinaryRainConfig.cpp
        ../scripts/src/BinaryRainEngine.cpp
        ../scripts/src/VideoPlayer.cpp
        ../scripts/src/D3DWallpaperRenderer.cpp
        ../scripts/include/VideoPlayer.h
        ../scripts/include/D3DWallpaperRenderer.h)
    target_include_directories(VideoPlayerRainTests PRIVATE ../scripts/include)
    target_link_libraries(VideoPlayerRainTests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
        Qt6::Multimedia Qt6::Concurrent d3d11 dxgi dcomp d3dcompiler ole32 User32 Gdi32 Dwmapi)
endif()

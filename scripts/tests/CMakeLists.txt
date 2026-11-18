cmake_minimum_required(VERSION 3.21)
project(MotivaTests LANGUAGES CXX)

# Standalone, opt-in unit tests (not part of the Motiva build). They compile
# only the Qt-light Binary Rain model and engine, so they need Qt Core/Gui/Test
# and no Multimedia, D3D or Win32 desktop code.
#   cmake -S scripts/tests -B <dir> -G Ninja -DCMAKE_PREFIX_PATH=<Qt kit>
#   cmake --build <dir> && <dir>/BinaryRainTests
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Test)

qt_add_executable(BinaryRainTests
    BinaryRainTests.cpp
    ../src/BinaryRainConfig.cpp
    ../src/BinaryRainEngine.cpp)
target_include_directories(BinaryRainTests PRIVATE ../include)
target_link_libraries(BinaryRainTests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)

qt_add_executable(DesktopScalingTests DesktopScalingTests.cpp)
target_include_directories(DesktopScalingTests PRIVATE ../include)
target_link_libraries(DesktopScalingTests PRIVATE Qt6::Core Qt6::Test)

qt_add_executable(VdDesktopTests VdDesktopTests.cpp)
target_include_directories(VdDesktopTests PRIVATE ../include)
target_link_libraries(VdDesktopTests PRIVATE Qt6::Core Qt6::Test)

# VideoPlayer lifecycle test: needs Qt Multimedia and the D3D libs (VideoPlayer
# links D3DWallpaperRenderer::tagAsStill). Skipped when Multimedia is absent.
find_package(Qt6 QUIET COMPONENTS Widgets Multimedia Concurrent)
if(TARGET Qt6::Multimedia AND TARGET Qt6::Widgets AND TARGET Qt6::Concurrent)
    qt_add_executable(VideoPlayerRainTests
        VideoPlayerRainTests.cpp
        ../src/BinaryRainConfig.cpp
        ../src/BinaryRainEngine.cpp
        ../src/VideoPlayer.cpp
        ../src/D3DWallpaperRenderer.cpp
        ../include/VideoPlayer.h
        ../include/D3DWallpaperRenderer.h)
    target_include_directories(VideoPlayerRainTests PRIVATE ../include)
    target_link_libraries(VideoPlayerRainTests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
        Qt6::Multimedia Qt6::Concurrent d3d11 dxgi dcomp d3dcompiler ole32 User32 Gdi32 Dwmapi)
endif()

# GUI regression test for the Virtual Desktop flicker fixes (in-place F11 restyle,
# covering-rect window, first-frame signal). Needs a real desktop + OpenGL; the
# test skips itself otherwise. Links the same sources the Motiva target uses.
find_package(Qt6 QUIET COMPONENTS OpenGL OpenGLWidgets Svg Sql Network)
if(TARGET Qt6::OpenGLWidgets AND TARGET Qt6::Multimedia AND TARGET Qt6::Widgets AND TARGET Qt6::Concurrent
   AND TARGET Qt6::Svg AND TARGET Qt6::Sql AND TARGET Qt6::Network)
    set(MOTIVA_SRC ../src)
    set(MOTIVA_INC ../include)
    qt_add_executable(VdWindowTests
        VdWindowTests.cpp
        ${MOTIVA_SRC}/VirtualDesktopWindow.cpp ${MOTIVA_INC}/VirtualDesktopWindow.h
        ${MOTIVA_SRC}/VdWallpaperSurface.cpp ${MOTIVA_INC}/VdWallpaperSurface.h
        ${MOTIVA_SRC}/VdDesktopChrome.cpp ${MOTIVA_INC}/VdDesktopChrome.h
        ${MOTIVA_SRC}/GlUi.cpp ${MOTIVA_INC}/GlUi.h
        ${MOTIVA_SRC}/GlWindow.cpp ${MOTIVA_INC}/GlWindow.h
        ${MOTIVA_SRC}/GlPopups.cpp ${MOTIVA_INC}/GlPopups.h
        ${MOTIVA_SRC}/GlColor.cpp ${MOTIVA_INC}/GlColor.h
        ${MOTIVA_SRC}/Theme.cpp ${MOTIVA_INC}/Theme.h
        ${MOTIVA_SRC}/ThemeCustomization.cpp
        ${MOTIVA_SRC}/PreviewSource.cpp ${MOTIVA_INC}/PreviewSource.h
        ${MOTIVA_SRC}/VideoPlayer.cpp ${MOTIVA_INC}/VideoPlayer.h
        ${MOTIVA_SRC}/D3DWallpaperRenderer.cpp ${MOTIVA_INC}/D3DWallpaperRenderer.h
        ${MOTIVA_SRC}/BinaryRainConfig.cpp ${MOTIVA_INC}/BinaryRainConfig.h
        ${MOTIVA_SRC}/BinaryRainEngine.cpp ${MOTIVA_INC}/BinaryRainEngine.h)
    target_include_directories(VdWindowTests PRIVATE ${MOTIVA_INC})
    target_link_libraries(VdWindowTests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets
        Qt6::Multimedia Qt6::Concurrent Qt6::Svg Qt6::Sql Qt6::Network Qt6::Test
        d3d11 dxgi dcomp d3dcompiler ole32 User32 Gdi32 Dwmapi)
endif()

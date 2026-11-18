@echo off
rem Builds Motiva, copies the exe to deployment\resources\bin (closes a running Motiva first) and launches it.
setlocal
rem Qt root: the folder that holds Tools\ and 6.12.0\. Override by setting QT_ROOT before running.
if not defined QT_ROOT set "QT_ROOT=C:\Qt"
if not exist "%QT_ROOT%\Tools\mingw1310_64\bin\g++.exe" if exist "D:\Apps\Qt6\Tools\mingw1310_64\bin\g++.exe" set "QT_ROOT=D:\Apps\Qt6"
if not exist "%QT_ROOT%\Tools\mingw1310_64\bin\g++.exe" (
    echo Qt toolchain not found under %QT_ROOT%. Set QT_ROOT to the folder containing Tools and 6.12.0.
    exit /b 1
)
set "PATH=%QT_ROOT%\Tools\mingw1310_64\bin;%QT_ROOT%\Tools\CMake_64\bin;%QT_ROOT%\Tools\Ninja;%QT_ROOT%\6.12.0\mingw_64\bin;%PATH%"
cd /d "%~dp0"
taskkill /F /IM Motiva.exe >nul 2>&1
cmake --build build || exit /b 1
copy /Y build\Motiva.exe deployment\resources\bin\Motiva.exe >nul || exit /b 1
echo Built and deployed.
start "" "deployment\resources\bin\Motiva.exe"

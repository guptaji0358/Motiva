@echo off
rem Builds Motiva and copies the exe to deployment\resources\bin (closes a running Motiva first).
setlocal
set "PATH=D:\Apps\Qt6\Tools\mingw1310_64\bin;D:\Apps\Qt6\6.11.1\mingw_64\bin;%PATH%"
cd /d "%~dp0"
taskkill /F /IM Motiva.exe >nul 2>&1
cmake --build build || exit /b 1
copy /Y build\Motiva.exe deployment\resources\bin\Motiva.exe >nul || exit /b 1
echo Built and deployed.

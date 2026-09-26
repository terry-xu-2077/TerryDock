@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
    echo [!] cmake not found in PATH.
    echo     Install "C++ CMake tools for Windows" from the Visual Studio Installer,
    echo     or run this from a Developer Command Prompt.
    exit /b 1
)

if not exist build mkdir build

cmake -G "Visual Studio 17 2022" -A x64 -B build -S .
if errorlevel 1 goto :fail

cmake --build build --config Release
if errorlevel 1 goto :fail

echo.
echo [OK] build\Release\LightDock.exe
exit /b 0

:fail
echo.
echo [X] Build failed.
exit /b 1

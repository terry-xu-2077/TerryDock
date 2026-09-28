@echo off
setlocal

rem Build with the bundled MinGW toolchain used by this project.
set "PROJECT_DIR=%~dp0"
set "PROJECT_ROOT=%PROJECT_DIR:~0,-1%"
set "TOOLCHAIN_DIR=%PROJECT_ROOT%\_toolchain\mingw\mingw64"
set "CMAKE=%TOOLCHAIN_DIR%\bin\cmake.exe"
set "BUILD_DIR=%PROJECT_ROOT%\build_mingw"

if not exist "%CMAKE%" (
    echo [!] Bundled CMake was not found:
    echo     %CMAKE%
    exit /b 1
)

set "PATH=%TOOLCHAIN_DIR%\bin;%PATH%"

"%CMAKE%" -G "MinGW Makefiles" -B "%BUILD_DIR%" -S "%PROJECT_ROOT%"
if errorlevel 1 goto :fail

"%CMAKE%" --build "%BUILD_DIR%" --parallel 4
if errorlevel 1 goto :fail

echo.
echo [OK] %BUILD_DIR%\LightDock.exe
exit /b 0

:fail
echo.
echo [X] Build failed.
exit /b 1

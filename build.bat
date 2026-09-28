@echo off
setlocal EnableExtensions

rem Build LightDock with the bundled MinGW toolchain used by this project.
set "PROJECT_DIR=%~dp0"
set "PROJECT_ROOT=%PROJECT_DIR:~0,-1%"
set "TOOLCHAIN_DIR=%PROJECT_ROOT%\_toolchain\mingw\mingw64"
set "CMAKE=%TOOLCHAIN_DIR%\bin\cmake.exe"
set "GXX=%TOOLCHAIN_DIR%\bin\g++.exe"
set "BUILD_DIR=%PROJECT_ROOT%\build_mingw"
set "OUTPUT_EXE=%BUILD_DIR%\LightDock.exe"

if not exist "%CMAKE%" (
    echo [!] Bundled CMake was not found:
    echo     %CMAKE%
    echo.
    echo Run setup-toolchain.bat first.
    goto :fail
)

if not exist "%GXX%" (
    echo [!] Bundled g++ was not found:
    echo     %GXX%
    echo.
    echo Run setup-toolchain.bat first.
    goto :fail
)

set "PATH=%TOOLCHAIN_DIR%\bin;%PATH%"

rem A stale cache from another generator/toolchain can make CMake configure
rem successfully in an unexpected layout or fail before the real build.
rem build_mingw belongs exclusively to this bundled MinGW build, so reset only
rem its CMake metadata when the cached generator is not MinGW Makefiles.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /b /c:"CMAKE_GENERATOR:INTERNAL=MinGW Makefiles" "%BUILD_DIR%\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo [INFO] Stale CMake cache detected. Resetting build_mingw metadata...
        if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt" >nul 2>nul
        if exist "%BUILD_DIR%\CMakeFiles" rmdir /s /q "%BUILD_DIR%\CMakeFiles"
    )
)

echo [1/3] Configuring LightDock...
"%CMAKE%" ^
    -G "MinGW Makefiles" ^
    -S "%PROJECT_ROOT%" ^
    -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=%BUILD_DIR%"
if errorlevel 1 goto :fail

echo.
echo [2/3] Building LightDock...
"%CMAKE%" --build "%BUILD_DIR%" --target LightDock --parallel 4
if errorlevel 1 goto :fail

echo.
echo [3/3] Verifying output...
if not exist "%OUTPUT_EXE%" (
    rem Be defensive in case an older CMake cache or target property placed the
    rem executable in a configuration subdirectory. Recover it to the documented
    rem location instead of claiming success with no visible EXE.
    set "FOUND_EXE="
    for /r "%BUILD_DIR%" %%F in (LightDock.exe) do (
        if not defined FOUND_EXE set "FOUND_EXE=%%~fF"
    )

    if defined FOUND_EXE (
        echo [INFO] Executable was emitted at:
        echo        %FOUND_EXE%
        echo [INFO] Copying it to the documented output path...
        copy /y "%FOUND_EXE%" "%OUTPUT_EXE%" >nul
        if errorlevel 1 goto :missing_output
    )
)

if not exist "%OUTPUT_EXE%" goto :missing_output

echo.
echo [OK] Build completed:
echo      %OUTPUT_EXE%
for %%F in ("%OUTPUT_EXE%") do echo      Size: %%~zF bytes
echo.
exit /b 0

:missing_output
echo.
echo [X] CMake reported a successful build, but LightDock.exe was not found.
echo     Expected:
echo     %OUTPUT_EXE%
echo.
echo     If this happens again, send me:
echo       1. build_mingw\CMakeCache.txt
echo       2. the complete build.bat console output
goto :fail

:fail
echo.
echo [X] Build failed.
echo.
pause
exit /b 1

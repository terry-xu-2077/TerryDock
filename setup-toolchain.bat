@echo off
setlocal EnableExtensions

rem Install the pinned MinGW-w64 + CMake toolchain used by build.bat.
set "PROJECT_DIR=%~dp0"
set "PROJECT_ROOT=%PROJECT_DIR:~0,-1%"
set "TOOLCHAIN_ROOT=%PROJECT_ROOT%\_toolchain"
set "MINGW_ROOT=%TOOLCHAIN_ROOT%\mingw"
set "CMAKE=%MINGW_ROOT%\mingw64\bin\cmake.exe"
set "GXX=%MINGW_ROOT%\mingw64\bin\g++.exe"
set "ARCHIVE=%TOOLCHAIN_ROOT%\winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip"
set "URL=https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-14.0.0-ucrt-r1/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip"
set "EXPECTED_SHA256=C1F52294597C0B73786B2A78EB5D176D89226D2F21875EAB75E783A8B1CEFCC4"

if exist "%CMAKE%" if exist "%GXX%" (
    echo [OK] Toolchain is already installed.
    "%CMAKE%" --version | findstr /b /c:"cmake version"
    "%GXX%" --version | findstr /b /c:"g++.exe"
    exit /b 0
)

if not exist "%TOOLCHAIN_ROOT%" mkdir "%TOOLCHAIN_ROOT%"
if not exist "%ARCHIVE%" (
    echo [INFO] Downloading the pinned WinLibs toolchain. This is about 261 MB.
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
        "Invoke-WebRequest -UseBasicParsing -Uri '%URL%' -OutFile '%ARCHIVE%'"
    if errorlevel 1 goto :download_failed
)

echo [INFO] Checking the downloaded archive...
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "%ARCHIVE%" SHA256') do if not defined HASH set "HASH=%%H"
set "HASH=%HASH: =%"
if /i not "%HASH%"=="%EXPECTED_SHA256%" (
    echo [X] SHA-256 verification failed.
    echo     Expected: %EXPECTED_SHA256%
    echo     Actual:   %HASH%
    exit /b 1
)

if exist "%MINGW_ROOT%" rmdir /s /q "%MINGW_ROOT%"
mkdir "%MINGW_ROOT%"
echo [INFO] Extracting the toolchain...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "Expand-Archive -LiteralPath '%ARCHIVE%' -DestinationPath '%MINGW_ROOT%' -Force"
if errorlevel 1 goto :extract_failed

del /q "%ARCHIVE%"
if not exist "%CMAKE%" goto :install_failed
if not exist "%GXX%" goto :install_failed

echo.
echo [OK] Toolchain installed in:
echo     %TOOLCHAIN_ROOT%
echo.
echo You can now double-click build.bat.
exit /b 0

:download_failed
echo [X] Toolchain download failed.
exit /b 1

:extract_failed
echo [X] Toolchain extraction failed.
exit /b 1

:install_failed
echo [X] Toolchain files were not found after extraction.
exit /b 1

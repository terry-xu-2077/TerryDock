@echo off
setlocal EnableExtensions

rem This launcher copies itself to %%TEMP%% before pulling so git can safely
rem update the repository copy while the current run continues unchanged.
if /i "%~1"=="--worker" goto :worker

set "SELF_COPY=%TEMP%\LightDock-update-build-run-%RANDOM%-%RANDOM%.bat"
copy /y "%~f0" "%SELF_COPY%" >nul
if errorlevel 1 (
    echo [X] Could not create the temporary launcher.
    pause
    exit /b 1
)

call "%SELF_COPY%" --worker "%~dp0"
set "RESULT=%ERRORLEVEL%"
del /q "%SELF_COPY%" >nul 2>nul
exit /b %RESULT%

:worker
set "PROJECT_DIR=%~2"
if not defined PROJECT_DIR goto :fail

pushd "%PROJECT_DIR%" >nul 2>nul
if errorlevel 1 (
    echo [X] Could not enter the LightDock project directory:
    echo     %PROJECT_DIR%
    goto :fail_no_pop
)

set "PROJECT_ROOT=%CD%"
set "OUTPUT_EXE=%PROJECT_ROOT%\build_mingw\LightDock.exe"

where git.exe >nul 2>nul
if errorlevel 1 (
    echo [X] git.exe was not found in PATH.
    echo     Install Git for Windows or add Git to PATH first.
    goto :fail
)

echo [1/4] Stopping running LightDock...
tasklist /FI "IMAGENAME eq LightDock.exe" /NH 2>nul | find /I "LightDock.exe" >nul
if errorlevel 1 (
    echo       LightDock is not running.
) else (
    taskkill /F /IM LightDock.exe >nul 2>nul
    if errorlevel 1 (
        echo [X] Could not stop the running LightDock.exe.
        goto :fail
    )
    echo       Stopped.
    timeout /t 1 /nobreak >nul
)

echo.
echo [2/4] Pulling latest main branch...
git pull --ff-only
if errorlevel 1 (
    echo.
    echo [X] git pull failed.
    echo     If you have local changes, commit or stash them first.
    goto :fail
)

echo.
echo [3/4] Building LightDock...
call "%PROJECT_ROOT%\build.bat" --no-pause
if errorlevel 1 goto :fail

if not exist "%OUTPUT_EXE%" (
    echo.
    echo [X] Build finished but LightDock.exe was not found:
    echo     %OUTPUT_EXE%
    goto :fail
)

echo.
echo [4/4] Launching LightDock...
start "" "%OUTPUT_EXE%"
if errorlevel 1 (
    echo [X] Failed to launch:
    echo     %OUTPUT_EXE%
    goto :fail
)

echo.
echo [OK] LightDock is updated, built, and running.
popd
exit /b 0

:fail
popd >nul 2>nul

:fail_no_pop
echo.
echo [X] Update / build / run did not complete.
echo.
pause
exit /b 1

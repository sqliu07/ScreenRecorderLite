@echo off
setlocal
pushd "%~dp0.."
if not exist "out\build\windows\Release\ScreenRecorderLite.exe" (
    call "%~dp0build.cmd"
    if errorlevel 1 (
        popd
        exit /b 1
    )
)
start "" "out\build\windows\Release\ScreenRecorderLite.exe"
popd

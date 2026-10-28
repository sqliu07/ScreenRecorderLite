@echo off
setlocal
pushd "%~dp0.."

if not exist "out\build\windows\Release\srl_probe.exe" (
    call "%~dp0build.cmd"
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

if not exist "artifacts\selftest" mkdir "artifacts\selftest"
"out\build\windows\Release\srl_probe.exe" selftest --out-dir artifacts\selftest > "artifacts\selftest\selftest.txt" 2>&1
set RESULT=%ERRORLEVEL%
type "artifacts\selftest\selftest.txt"
echo.
echo Self-test exit code: %RESULT%
echo Log: artifacts\selftest\selftest.txt
popd
exit /b %RESULT%

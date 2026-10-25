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

if not exist "artifacts\m0_selftest" mkdir "artifacts\m0_selftest"
"out\build\windows\Release\srl_probe.exe" selftest --out-dir artifacts\m0_selftest > "artifacts\m0_selftest\selftest.txt" 2>&1
set RESULT=%ERRORLEVEL%
type "artifacts\m0_selftest\selftest.txt"
echo.
echo Self-test exit code: %RESULT%
echo Log: artifacts\m0_selftest\selftest.txt
popd
exit /b %RESULT%

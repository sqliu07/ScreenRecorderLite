@echo off
setlocal
pushd "%~dp0.."

set "SRL_FRESH_CONFIGURE="
set "SRL_SKIP_TESTS="
:parse_args
if "%~1"=="" goto :args_parsed
if /I "%~1"=="--fresh" (
    set "SRL_FRESH_CONFIGURE=--fresh"
    shift /1
    goto :parse_args
)
if /I "%~1"=="--skip-tests" (
    set "SRL_SKIP_TESTS=1"
    shift /1
    goto :parse_args
)
echo Usage: scripts\build.cmd [--fresh] [--skip-tests]
goto :fail

:args_parsed

if not defined VCPKG_ROOT (
    echo [error] VCPKG_ROOT must point to your vcpkg installation.
    echo         Install vcpkg and set VCPKG_ROOT before building.
    popd
    exit /b 1
)

if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo [error] VCPKG_ROOT does not contain the vcpkg CMake toolchain.
    goto :fail
)
if not exist "%VCPKG_ROOT%\.git" (
    echo [error] This project requires a full Git checkout of vcpkg for its pinned baseline.
    echo         Use git clone to install vcpkg, then set VCPKG_ROOT to that directory.
    goto :fail
)

set "SRL_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%SRL_VSWHERE%" (
    echo [error] MSVC C++ Build Tools were not found.
    echo         Install the C++ desktop build tools and Windows SDK.
    goto :fail
)
set "SRL_MSVC_INSTALL="
for /f "usebackq delims=" %%i in (`"%SRL_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "SRL_MSVC_INSTALL=%%i"
if not defined SRL_MSVC_INSTALL (
    echo [error] No MSVC installation with C++ desktop tools was found.
    goto :fail
)
set "SRL_VCPKG_ROOT=%VCPKG_ROOT%"
call "%SRL_MSVC_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 goto :fail
set "VCPKG_ROOT=%SRL_VCPKG_ROOT%"
cd /d "%~dp0.."

where cmake >nul 2>nul
if errorlevel 1 (
    echo [error] Install CMake 3.25+ and add it to PATH.
    goto :fail
)
where ninja >nul 2>nul
if errorlevel 1 if exist "%SRL_MSVC_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%SRL_MSVC_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where ninja >nul 2>nul
if errorlevel 1 (
    echo [error] Install Ninja or the C++ CMake tools component and add Ninja to PATH.
    goto :fail
)

echo Using MSVC tools from: %SRL_MSVC_INSTALL%
cmake %SRL_FRESH_CONFIGURE% --preset windows
if errorlevel 1 goto :fail

cmake --build --preset release
if errorlevel 1 goto :fail

if not defined SRL_SKIP_TESTS (
    ctest --preset release
    if errorlevel 1 goto :fail
)

echo.
echo Release build succeeded.
echo   app : out\build\windows\Release\ScreenRecorderLite.exe
popd
exit /b 0

:fail
echo.
echo [error] build failed.
popd
exit /b 1

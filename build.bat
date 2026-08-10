@echo off
setlocal EnableDelayedExpansion

rem ============================================================================
rem  TradeBox build script
rem
rem  Locates a complete Visual Studio installation with the C++ workload, loads
rem  its x64 MSVC environment, and configures/builds with CMake + Ninja.
rem  Safe to run from any shell; no VS developer prompt or manual PATH setup.
rem
rem  Usage:
rem    build.bat              build the TradeBoxNative app (Release)
rem    build.bat Debug        build the app with the Debug configuration
rem    build.bat Release test build the app, then build and run the test suite
rem                           (requires Python 3 on PATH)
rem
rem  The script reconfigures from scratch (cmake --fresh) only when the
rem  detected toolchain differs from the one recorded in build/CMakeCache.txt,
rem  so a stale cache can never silently poison a build.
rem ============================================================================

cd /d "%~dp0"

set "BUILD_CONFIG=%~1"
if not defined BUILD_CONFIG set "BUILD_CONFIG=Release"
set "RUN_TESTS=%~2"

rem ---- locate a Visual Studio installation with the C++ workload ---------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_18="
set "VS_ANY="

if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%V in (`"%VSWHERE%" -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if not defined VS_ANY set "VS_ANY=%%V"
    echo %%V| findstr /i /c:"\18\Insiders" >nul
    if not errorlevel 1 if not defined VS_18 set "VS_18=%%V"
  )
)

set "VS_ROOT="
if defined VS_18 (
  set "VS_ROOT=!VS_18!"
) else if defined VS_ANY (
  set "VS_ROOT=!VS_ANY!"
) else (
  rem vswhere unavailable or empty: probe conventional install layouts
  call :try_vs "C:\Program Files\Microsoft Visual Studio\18\Insiders"
  call :try_vs "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Community"
  call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
)

if not defined VS_ROOT (
  echo.
  echo [build] ERROR: no Visual Studio installation with the C++ workload was found.
  echo         Install "Desktop development with C++" through the Visual Studio
  echo         Installer, then run build.bat again.
  echo.
  exit /b 2
)

rem ---- initialize the MSVC x64 environment -------------------------------------
set "VCVARS64=!VS_ROOT!\VC\Auxiliary\Build\vcvars64.bat"
call "!VCVARS64!" >nul
if errorlevel 1 (
  echo [build] ERROR: failed to initialize the MSVC environment from:
  echo         !VCVARS64!
  exit /b 3
)
echo [build] Toolchain: !VS_ROOT!

rem Compiler-affecting variables from the caller's shell must not leak in.
rem (A foreign/unquoted CL path in the environment was the original cause of
rem the "Cannot open source file: 'C:\Program'" failures.)
set "CL="
set "_CL_="
set "CFLAGS="
set "CXXFLAGS="

rem ---- locate cl, ninja, cmake -------------------------------------------------
set "CL_EXE="
for /f "delims=" %%C in ('where cl 2^>nul') do if not defined CL_EXE set "CL_EXE=%%C"
if not defined CL_EXE (
  echo [build] ERROR: cl.exe not found after loading the MSVC environment.
  exit /b 4
)

set "NINJA=!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "!NINJA!" (
  set "NINJA="
  for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%N"
)
if not defined NINJA (
  echo [build] ERROR: Ninja generator not found. Install "C++ CMake tools for
  echo         Windows" in the Visual Studio Installer, or add ninja.exe to PATH.
  exit /b 5
)

set "CMAKE_EXE="
for /f "delims=" %%M in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%M"
if not defined CMAKE_EXE set "CMAKE_EXE=!VS_ROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "!CMAKE_EXE!" (
  echo [build] ERROR: CMake not found. Install "C++ CMake tools for Windows" in
  echo         the Visual Studio Installer, or add cmake.exe to PATH.
  exit /b 6
)

rem ---- detect a stale build cache ----------------------------------------------
rem Keeps the normal dev loop fast (no forced reconfigure every run) while never
rem building on top of a cache recorded by a different compiler.
set "STALE=0"
set "CACHE_CL="
if exist "build\CMakeCache.txt" (
  for /f "usebackq delims=" %%L in (`findstr /b /c:"CMAKE_CXX_COMPILER:STRING=" build\CMakeCache.txt 2^>nul`) do set "CACHE_LINE=%%L"
  if defined CACHE_LINE (
    set "CACHE_CL=!CACHE_LINE:*CMAKE_CXX_COMPILER:STRING==!"
  )
  set "CL_EXE_N=!CL_EXE:\=/!"
  if not defined CACHE_CL (
    set "STALE=1"
  ) else (
    if /i not "!CACHE_CL!"=="!CL_EXE_N!" set "STALE=1"
  )
) else (
  set "STALE=1"
)

set "FRESH="
if "!STALE!"=="1" (
  echo [build] Toolchain changed since last configure; reconfiguring from scratch.
  set "FRESH=--fresh"
)

set "TEST_FLAG=-DBUILD_TESTING=OFF"
if /i "!RUN_TESTS!"=="test" set "TEST_FLAG=-DBUILD_TESTING=ON"

rem ---- configure and build ------------------------------------------------------
set "RUNNING_APP="
for /f "tokens=1" %%P in ('tasklist /FI "IMAGENAME eq TradeBoxNative.exe" /FO CSV /NH 2^>nul ^| find /I "TradeBoxNative.exe"') do if not defined RUNNING_APP set "RUNNING_APP=%%~P"
if defined RUNNING_APP (
  echo [build] Stopping running TradeBoxNative.exe, PID !RUNNING_APP! ...
  taskkill /PID !RUNNING_APP! /T /F >nul 2>&1
  if errorlevel 1 (
    echo [build] ERROR: could not stop TradeBoxNative.exe.
    exit /b 7
  )
  rem Give Windows a moment to release the executable and dependent files.
  ping 127.0.0.1 -n 2 >nul
)
echo [build] Configuring (Ninja, !BUILD_CONFIG!) ...
"%CMAKE_EXE%" -S . -B build -G Ninja !FRESH! !TEST_FLAG! -DCMAKE_BUILD_TYPE=!BUILD_CONFIG! -DCMAKE_C_COMPILER="!CL_EXE!" -DCMAKE_CXX_COMPILER="!CL_EXE!" -DCMAKE_MAKE_PROGRAM="!NINJA!"
if errorlevel 1 exit /b 7

echo [build] Building !BUILD_CONFIG! ...
"%CMAKE_EXE%" --build build --config !BUILD_CONFIG!
if errorlevel 1 exit /b 8

if /i "!RUN_TESTS!"=="test" (
  set "CTEST_EXE="
  for /f "delims=" %%T in ('where ctest 2^>nul') do if not defined CTEST_EXE set "CTEST_EXE=%%T"
  if not defined CTEST_EXE for %%F in ("!CMAKE_EXE!") do if exist "%%~dpFctest.exe" set "CTEST_EXE=%%~dpFctest.exe"
  if not defined CTEST_EXE (
    echo [build] ERROR: ctest not found for the test run.
    exit /b 9
  )
  echo [build] Running tests ...
  "!CTEST_EXE!" --test-dir build -C !BUILD_CONFIG! --output-on-failure
  if errorlevel 1 exit /b 10
)

echo [build] Done.
exit /b 0

:try_vs
if defined VS_ROOT goto :eof
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VS_ROOT=%~1"
goto :eof

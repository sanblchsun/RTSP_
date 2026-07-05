@echo off
REM chcp 65001 >nul 2>&1  REM GNU Make 3.81 несовместим с UTF-8 кодировкой консоли
setlocal enabledelayedexpansion

REM ============================================================
REM build.bat — Automated build of libx264 (SIMD) + desktop_streamer
REM ============================================================

set X264_SRC=%~dp0x264_build
set X264_SDK=C:\x264_sdk
set PROJECT_DIR=%~dp0
set BUILD_DIR=%PROJECT_DIR%build64

REM ---- Add tools to PATH ----
set "PATH=C:\Program Files\NASM;%PATH%"
set "PATH=C:\Program Files (x86)\GnuWin32\bin;%PATH%"

REM ---- 1. Find Visual Studio (2026 -> 2022 -> 2019) ----
set VS_YEARS=18 17 16
set VS_EDITION=Community
set FOUND_VS=

for %%Y in (%VS_YEARS%) do (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\%%Y\%VS_EDITION%\VC\Auxiliary\Build\vcvars64.bat"
    if exist "!VCVARS!" (
        set FOUND_VS=!VCVARS!
        goto :found_vs
    )
)
echo [BUILD] Visual Studio x64 tools not found!
echo         Install Visual Studio 2022/2026 with "Desktop development with C++"
exit /b 1

:found_vs
echo [BUILD] Found: %FOUND_VS%

REM ---- 2. Check prerequisites ----
nasm --version >nul 2>&1
if errorlevel 1 (
    echo [BUILD] nasm not found! Install nasm:
    echo         winget install nasm
    exit /b 1
)

make --version >nul 2>&1
if errorlevel 1 (
    echo [BUILD] GNU make not found! Install GnuWin32 make or MSYS2.
    echo         Download: http://gnuwin32.sourceforge.net/packages/make.htm
    exit /b 1
)

REM ---- 3. Build libx264 with SIMD assembly ----
echo ============================================================
echo [BUILD] Step 1/2: Building libx264 with SIMD (64-bit, nasm)
echo ============================================================
call "%FOUND_VS%"

cd /d "%X264_SRC%"
if errorlevel 1 (
    echo [BUILD] x264 source not found at %X264_SRC%
    exit /b 1
)

echo [BUILD] Cleaning previous build...
make clean 2>nul

echo [BUILD] Compiling libx264 (this may take a few minutes)...
make lib-static
if errorlevel 1 (
    echo [BUILD] libx264 build FAILED!
    exit /b 1
)

echo [BUILD] Copying to SDK...
if not exist "%X264_SDK%\lib\x64" mkdir "%X264_SDK%\lib\x64"
if not exist "%X264_SDK%\include" mkdir "%X264_SDK%\include"
copy /Y libx264.lib "%X264_SDK%\lib\x64\libx264.lib" >nul
copy /Y x264.h "%X264_SDK%\include\x264.h" >nul
copy /Y x264_config.h "%X264_SDK%\include\x264_config.h" >nul
echo [BUILD] libx264 SDK updated: libx264.lib (SIMD enabled)

REM ---- 4. Build desktop_streamer.exe ----
echo ============================================================
echo [BUILD] Step 2/2: Building desktop_streamer.exe
echo ============================================================
cd /d "%PROJECT_DIR%"

REM Re-run cmake to regenerate if needed
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [BUILD] Configuring CMake...
    cmake -B "%BUILD_DIR%" -A x64
)

echo [BUILD] Compiling desktop_streamer.exe...
cmake --build "%BUILD_DIR%" --config Release --clean-first
if errorlevel 1 (
    echo [BUILD] desktop_streamer build FAILED!
    exit /b 1
)

echo ============================================================
echo [BUILD] SUCCESS!
echo ============================================================
echo Binary: %BUILD_DIR%\Release\desktop_streamer.exe
echo.
echo Verify no VC redist dependencies:
echo     dumpbin /dependents "%BUILD_DIR%\Release\desktop_streamer.exe"
echo.
echo Run:
echo     desktop_streamer.exe push ^<VPS_IP^>
echo.
@echo off
setlocal enabledelayedexpansion

echo ===============================================
echo RTSP Desktop Agent — Installation
echo ===============================================
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Must be run as administrator
    echo Please right-click and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

set "INSTALL_DIR=C:\ProgramData\rtsp-agent"

echo [1/4] Creating directory: %INSTALL_DIR%
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if %errorlevel% neq 0 (
    echo ERROR: Failed to create %INSTALL_DIR%
    pause
    exit /b 1
)

echo [2/4] Copying agent.exe...
copy /Y "%~dp0agent.exe" "%INSTALL_DIR%\agent.exe"
if %errorlevel% neq 0 (
    echo ERROR: Failed to copy agent.exe
    pause
    exit /b 1
)

echo [3/4] Copying uninstall.cmd...
copy /Y "%~dp0uninstall.cmd" "%INSTALL_DIR%\uninstall.cmd"
if %errorlevel% neq 0 (
    echo WARNING: Failed to copy uninstall.cmd
)

echo [4/4] Installing service...
"%INSTALL_DIR%\agent.exe" --install
if %errorlevel% equ 0 (
    echo.
    echo ===============================================
    echo Installation complete
    echo ===============================================
    echo Path: %INSTALL_DIR%
    echo Service: RTSPDesktopAgent
    echo.
) else (
    echo.
    echo ERROR: Service installation failed (code: %errorlevel%)
    pause
    exit /b 1
)

sc query RTSPDesktopAgent >nul 2>&1
if %errorlevel% equ 0 (
    echo Service status:
    sc query RTSPDesktopAgent | findstr STATE
) else (
    echo WARNING: Service not found after installation
)

echo.
echo Press any key to exit...
pause >nul
exit /b 0

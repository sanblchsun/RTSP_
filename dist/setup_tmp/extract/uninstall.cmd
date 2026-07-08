@echo off
setlocal enabledelayedexpansion

echo ===============================================
echo RTSP Desktop Agent — Uninstall
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

echo [1/4] Stopping service...
sc stop RTSPDesktopAgent >nul 2>&1
echo OK

echo [2/4] Removing service...
sc delete RTSPDesktopAgent >nul 2>&1
echo OK

echo [3/4] Killing agent processes...
taskkill /f /im agent.exe >nul 2>&1
echo OK

echo [4/4] Deleting files...
if exist "%INSTALL_DIR%" (
    rmdir /s /q "%INSTALL_DIR%"
    if %errorlevel% equ 0 (
        echo Deleted: %INSTALL_DIR%
    ) else (
        echo WARNING: Could not delete %INSTALL_DIR% entirely.
        echo Some files may be in use. Please reboot and try again.
    )
) else (
    echo Directory not found: %INSTALL_DIR%
)

echo.
echo ===============================================
echo Uninstall complete
echo ===============================================
echo.
pause
exit /b 0

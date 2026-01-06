@echo off
title MotionCam Fuse - Disable ProjectedFS
color 1F
cls
echo ================================================
echo  Disabling Windows Projected File System
echo ================================================
echo.
echo This will disable the Windows Projected File System feature.
echo Administrator privileges are required.
echo.
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo This script must be run as administrator.
    echo Right-click and choose "Run as administrator".
    echo.
    pause
    exit /b 1
)
echo Disabling feature...
dism /online /disable-feature /featurename:Client-ProjFS /norestart
if %errorLevel% neq 0 (
    echo.
    echo ================================================
    echo  Failed
    echo ================================================
    echo.
    echo The feature could not be disabled.
    echo.
    pause
    exit /b 1
)
echo.
echo ================================================
echo  Done!
echo ================================================
echo.
echo The feature has been disabled.
echo You may need to restart your computer for changes to take effect.
echo.
timeout /t 3 >nul

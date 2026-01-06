@echo off
title MotionCam Fuse - Enable ProjectedFS
color 1F
cls
echo ================================================
echo  Enabling Windows Projected File System
echo ================================================
echo.
echo This will enable the Windows Projected File System feature.
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
echo Enabling feature...
dism /online /enable-feature /featurename:Client-ProjFS /norestart
if %errorLevel% neq 0 (
    echo.
    echo ================================================
    echo  Failed
    echo ================================================
    echo.
    echo The feature could not be enabled.
    echo.
    pause
    exit /b 1
)
echo.
echo ================================================
echo  Done!
echo ================================================
echo.
echo The feature has been enabled.
echo You may need to restart your computer for changes to take effect.
echo.
timeout /t 3 >nul

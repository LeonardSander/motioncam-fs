@echo off
REM MotionCam Fuse Clean Script
REM This script removes the build directory

echo ======================================
echo MotionCam Fuse Clean Script
echo ======================================
echo.

if not exist "build" (
    echo Build directory does not exist. Nothing to clean.
    pause
    exit /b 0
)

echo WARNING: This will delete the entire build directory.
set /p confirm="Are you sure? (y/N): "

if /i not "%confirm%"=="y" (
    echo Clean cancelled.
    pause
    exit /b 0
)

echo Cleaning build directory...
rmdir /s /q build

if errorlevel 1 (
    echo ERROR: Failed to remove build directory
    pause
    exit /b 1
)

echo.
echo ======================================
echo Clean completed successfully!
echo ======================================
echo.
pause

@echo off
REM MotionCam Fuse Rebuild Script
REM This script rebuilds the project (cleans and builds)

echo ======================================
echo MotionCam Fuse Rebuild Script
echo ======================================
echo.

if not exist "build" (
    echo Build directory does not exist. Running initial build...
    call build.bat
    exit /b
)

echo Rebuilding project...
cd build
cmake --build . --config Release --clean-first

if errorlevel 1 (
    echo.
    echo ERROR: Rebuild failed
    cd ..
    pause
    exit /b 1
)

cd ..
echo.
echo ======================================
echo Rebuild completed successfully!
echo ======================================
echo.
echo Executable location: build\Release\MotionCamFuse.exe
echo.
pause

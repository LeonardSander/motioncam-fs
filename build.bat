@echo off
REM MotionCam Fuse Build Script
REM This script builds the MotionCam Fuse project with all dependencies

echo ======================================
echo MotionCam Fuse Build Script
echo ======================================
echo.

REM Set paths
set VCPKG_ROOT=C:\vcpkg
set QT_DIR=C:\Qt\6.9.0\msvc2022_64
set WINDOWS_SDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64

REM Check if motioncam-decoder submodule exists
echo Checking dependencies...
if not exist "deps\motioncam-decoder\lib" (
    echo Cloning motioncam-decoder submodule...
    git clone https://github.com/LeonardSander/motioncam-decoder.git deps/motioncam-decoder
    if errorlevel 1 (
        echo ERROR: Failed to clone motioncam-decoder
        pause
        exit /b 1
    )
) else (
    echo motioncam-decoder already exists
)
echo.

REM Check if vcpkg exists
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo ERROR: vcpkg not found at %VCPKG_ROOT%
    echo Please install vcpkg or update VCPKG_ROOT path in this script
    pause
    exit /b 1
)
echo vcpkg found at %VCPKG_ROOT%
echo.

REM Check if Qt exists
if not exist "%QT_DIR%\bin\qmake.exe" (
    echo ERROR: Qt not found at %QT_DIR%
    echo Please install Qt 6.9.0 MSVC 2022 64-bit or update QT_DIR path in this script
    pause
    exit /b 1
)
echo Qt found at %QT_DIR%
echo.

REM Create build directory
if not exist "build" (
    echo Creating build directory...
    mkdir build
)

REM Configure with CMake
echo ======================================
echo Configuring CMake...
echo ======================================
cd build
cmake .. ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    -DCMAKE_LIBRARY_PATH="%WINDOWS_SDK_LIB%"

if errorlevel 1 (
    echo.
    echo ERROR: CMake configuration failed
    cd ..
    pause
    exit /b 1
)
echo.

REM Build the project
echo ======================================
echo Building project...
echo ======================================
cmake --build . --config Release

if errorlevel 1 (
    echo.
    echo ERROR: Build failed
    cd ..
    pause
    exit /b 1
)

cd ..
echo.
echo ======================================
echo Build completed successfully!
echo ======================================
echo.
echo Executable location: build\Release\MotionCamFuse.exe
echo.
pause

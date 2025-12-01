#!/bin/bash
# Build script for WSL using Windows Qt installation

set -e

# Configuration
BUILD_TYPE="${1:-Debug}"  # Default to Debug, can pass Release as argument
BUILD_DIR="build"
QT_PATH="/mnt/c/Qt/6.9.1/mingw_64"

# Check if Qt exists
if [ ! -d "$QT_PATH" ]; then
    echo "Error: Qt not found at $QT_PATH"
    echo "Please install Qt for Windows or update QT_PATH in this script"
    exit 1
fi

echo "=== Building MotionCam Fuse in WSL ==="
echo "Build directory: $BUILD_DIR"
echo "Build type: $BUILD_TYPE"
echo "Qt path: $QT_PATH"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo "Configuring with CMake..."

# Remove CMake cache to force reconfiguration
rm -f CMakeCache.txt

cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$QT_PATH" \
    -DQt6_DIR="$QT_PATH/lib/cmake/Qt6" \
    -DQt6CoreTools_DIR="$QT_PATH/lib/cmake/Qt6CoreTools" \
    -DQt6GuiTools_DIR="$QT_PATH/lib/cmake/Qt6GuiTools" \
    -DQt6WidgetsTools_DIR="$QT_PATH/lib/cmake/Qt6WidgetsTools" \
    -DCMAKE_FIND_ROOT_PATH="$QT_PATH" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -G Ninja \
    ..

# Build
echo ""
echo "Building..."
ninja -j$(nproc)

echo ""
echo "=== Build Complete ==="
echo "Executable: $BUILD_DIR/$BUILD_TYPE/MotionCamFuse"
echo ""
echo "To debug with GDB:"
echo "  gdb ./$BUILD_DIR/$BUILD_TYPE/MotionCamFuse"
echo ""
echo "Or use VSCode 'MotionCam FS Debug (WSL + GDB)' launch config"
echo ""
echo "To build Release version:"
echo "  ./build-wsl.sh Release"

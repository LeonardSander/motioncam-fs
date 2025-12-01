#!/bin/bash
# WSL Ubuntu setup script for MotionCam Fuse

set -e

echo "=== MotionCam Fuse WSL Setup ==="

# Update package lists
echo "Updating package lists..."
sudo apt-get update

# Install build essentials
echo "Installing build tools..."
sudo apt-get install -y \
    build-essential \
    clang \
    lldb \
    cmake \
    ninja-build \
    git \
    pkg-config \
    curl \
    zip \
    unzip \
    tar \
    gdb \
    nasm \
    yasm \
    autoconf \
    automake \
    libtool

# Note: We'll use Windows Qt installation instead of WSL Qt
# This avoids version mismatches and allows GUI to work properly
echo "Skipping WSL Qt installation - will use Windows Qt from /mnt/c/Qt"

# Install vcpkg if not present
if [ ! -d "$HOME/vcpkg" ]; then
    echo "Installing vcpkg..."
    cd ~
    git clone https://github.com/Microsoft/vcpkg.git
    cd vcpkg
    ./bootstrap-vcpkg.sh
    echo "export VCPKG_ROOT=$HOME/vcpkg" >> ~/.bashrc
    echo "export PATH=\$VCPKG_ROOT:\$PATH" >> ~/.bashrc
else
    echo "vcpkg already installed"
fi

export VCPKG_ROOT=$HOME/vcpkg
export PATH=$VCPKG_ROOT:$PATH

# Install FFmpeg build dependencies
echo "Installing FFmpeg build dependencies..."
sudo apt-get install -y \
    nasm \
    yasm

# Enable binary caching for faster builds
echo "Configuring vcpkg binary caching..."
export VCPKG_BINARY_SOURCES="clear;default,readwrite"

# Install vcpkg dependencies (use multiple cores)
echo "Installing vcpkg dependencies (this may take a while on first run)..."
cd "$VCPKG_ROOT"

# Update vcpkg to get latest binary cache
git pull

# Install with parallel builds
./vcpkg install \
    boost-algorithm \
    boost-filesystem \
    boost-locale \
    boost-iostreams \
    spdlog \
    bshoshany-thread-pool \
    ffmpeg[avcodec,avformat,swscale] \
    --clean-after-build

echo ""
echo "=== Setup Complete ==="
echo "WSL environment ready for building MotionCam Fuse"
echo ""
echo "Next steps:"
echo "  1. Ensure Windows Qt is installed (e.g., /mnt/c/Qt/6.9.1/mingw_64)"
echo "  2. Update QT_PATH in build-wsl.sh if needed"
echo "  3. Run: ./build-wsl.sh"
echo ""
echo "The build will use:"
echo "  - Windows Qt installation (for GUI compatibility)"
echo "  - WSL GCC/Clang (for DWARF debug symbols)"
echo "  - vcpkg dependencies from WSL"
echo ""
echo "For debugging:"
echo "  - Use VSCode 'MotionCam FS Debug (WSL + GDB)' launch config"
echo "  - Or run: gdb ./build-wsl-debug/MotionCamFuse"

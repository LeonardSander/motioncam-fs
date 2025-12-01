# Build script for Windows with Clang
param(
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

# Configuration
$BUILD_DIR = "build"
$QT_PATH = "C:\Qt\6.9.1\msvc2022_64"

Write-Host "=== Building MotionCam Fuse with Clang ===" -ForegroundColor Cyan
Write-Host "Build type: $BuildType" -ForegroundColor Yellow
Write-Host "Qt path: $QT_PATH" -ForegroundColor Yellow
Write-Host ""

# Find Clang
$CLANG_PATH = $null
if (Get-Command clang -ErrorAction SilentlyContinue) {
    $CLANG_PATH = "clang"
} elseif (Test-Path "C:\Program Files\LLVM\bin\clang.exe") {
    $CLANG_PATH = "C:\Program Files\LLVM\bin\clang.exe"
    $env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
} else {
    Write-Host "Error: Clang not found. Please install LLVM." -ForegroundColor Red
    Write-Host "Download from: https://github.com/llvm/llvm-project/releases" -ForegroundColor Yellow
    exit 1
}

Write-Host "Using Clang: $CLANG_PATH" -ForegroundColor Green

# Check if Qt exists
if (-not (Test-Path $QT_PATH)) {
    Write-Host "Error: Qt not found at $QT_PATH" -ForegroundColor Red
    exit 1
}

# Create build directory
if (-not (Test-Path $BUILD_DIR)) {
    New-Item -ItemType Directory -Path $BUILD_DIR | Out-Null
}

Set-Location $BUILD_DIR

# Configure with CMake
Write-Host "Configuring with CMake..." -ForegroundColor Cyan
cmake `
    -DCMAKE_BUILD_TYPE="$BuildType" `
    -DCMAKE_C_COMPILER=clang `
    -DCMAKE_CXX_COMPILER=clang++ `
    -DCMAKE_PREFIX_PATH="$QT_PATH" `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$BuildType" `
    -G Ninja `
    ..

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration failed" -ForegroundColor Red
    exit 1
}

# Build
Write-Host ""
Write-Host "Building..." -ForegroundColor Cyan
ninja

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed" -ForegroundColor Red
    exit 1
}

Set-Location ..

Write-Host ""
Write-Host "=== Build Complete ===" -ForegroundColor Green
Write-Host "Executable: $BUILD_DIR\$BuildType\MotionCamFuse.exe" -ForegroundColor Green
Write-Host ""
Write-Host "To debug with LLDB, use VSCode 'MotionCam FS Debug (Clang + LLDB)' config" -ForegroundColor Cyan

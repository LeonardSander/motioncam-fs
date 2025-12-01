# Windows Clang setup script for MotionCam Fuse
# This sets up Clang/LLVM for native Windows builds with DWARF debug symbols

Write-Host "=== MotionCam Fuse Windows Clang Setup ===" -ForegroundColor Cyan

# Check if Clang is installed
$clangPath = Get-Command clang -ErrorAction SilentlyContinue

if (-not $clangPath) {
    Write-Host "Clang not found. Installing via winget..." -ForegroundColor Yellow
    
    # Try to install LLVM via winget
    try {
        winget install -e --id LLVM.LLVM
        Write-Host "LLVM installed. Please restart your terminal and run this script again." -ForegroundColor Green
        exit 0
    } catch {
        Write-Host "Failed to install via winget. Please install manually:" -ForegroundColor Red
        Write-Host "  1. Download from: https://github.com/llvm/llvm-project/releases" -ForegroundColor Yellow
        Write-Host "  2. Or use: choco install llvm" -ForegroundColor Yellow
        exit 1
    }
}

Write-Host "Clang found at: $($clangPath.Source)" -ForegroundColor Green

# Check Clang version
$clangVersion = & clang --version | Select-Object -First 1
Write-Host "Version: $clangVersion" -ForegroundColor Green

# Check if vcpkg is set up
if (-not (Test-Path "vcpkg_installed")) {
    Write-Host "`nInstalling vcpkg dependencies..." -ForegroundColor Yellow
    
    # Run vcpkg install
    if (Test-Path "vcpkg.json") {
        cmake -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
              -DCMAKE_C_COMPILER=clang `
              -DCMAKE_CXX_COMPILER=clang++ `
              -G Ninja
    } else {
        Write-Host "vcpkg.json not found!" -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n=== Setup Complete ===" -ForegroundColor Green
Write-Host "You can now build with Clang using:" -ForegroundColor Cyan
Write-Host "  cmake -B build-clang -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja" -ForegroundColor White
Write-Host "  cmake --build build-clang --config Debug" -ForegroundColor White

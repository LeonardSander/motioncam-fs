# Configure using the same Qt, vcpkg triplets and Vulkan policy as the build.
param(
    [string]$QtPath = $env:QT_ROOT_DIR,
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/scripts/build_windows.ps1" -Compiler Clang -Phase Configure @PSBoundParameters
exit $LASTEXITCODE

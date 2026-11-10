# Compatibility entry point for local clang-cl builds.
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$BuildType = 'Release',
    [string]$QtPath = $env:QT_ROOT_DIR,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$BuildDirectory = 'build-windows-clang',
    [ValidateSet('All', 'Configure', 'Build', 'Rebuild', 'PrintConfig')]
    [string]$Phase = 'All'
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/scripts/build_windows.ps1" -Compiler Clang @PSBoundParameters
exit $LASTEXITCODE

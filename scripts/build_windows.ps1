# Shared Windows configuration for local builds and CI.
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$BuildType = 'Release',
    [ValidateSet('MSVC', 'Clang')]
    [string]$Compiler = 'MSVC',
    [string]$QtPath = $env:QT_ROOT_DIR,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$BuildDirectory = '',
    [string]$ProjectedFsLibrary = '',
    [ValidateSet('All', 'Configure', 'Build', 'Rebuild', 'PrintConfig')]
    [string]$Phase = 'All',
    [int]$Parallel = 0
)

$ErrorActionPreference = 'Stop'
$sourceDirectory = Split-Path $PSScriptRoot -Parent
$qtVersion = if ($env:QT_VERSION) { $env:QT_VERSION } else { '6.12.0' }
if (-not $VcpkgRoot) { $VcpkgRoot = 'C:\vcpkg' }
$VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)
if (-not $BuildDirectory) {
    $BuildDirectory = if ($Compiler -eq 'Clang') { 'build-windows-clang' } else { 'build-windows' }
}
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $sourceDirectory $BuildDirectory
}
$cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
$cleanFirst = $Phase -eq 'Rebuild'
if ($cleanFirst) {
    $Phase = if (Test-Path $cachePath) { 'Build' } else { 'All' }
}
if ($Phase -eq 'Build' -and -not $PSBoundParameters.ContainsKey('QtPath')) {
    # Reuse the kit that produced this tree, even if the ambient Qt changed.
    $cachedQt = Select-String -Path $cachePath -Encoding UTF8 `
        -Pattern '^Qt6_DIR:[^=]+=(.+)$' | Select-Object -First 1
    if (-not $cachedQt) { throw "Qt6_DIR is missing from $cachePath; configure the build first." }
    $QtPath = Join-Path $cachedQt.Matches[0].Groups[1].Value '../../..'
}
if (-not $QtPath) { $QtPath = "C:\Qt\$qtVersion\msvc2022_64" }
$QtPath = [IO.Path]::GetFullPath($QtPath)
$env:PATH = "$QtPath/bin;$env:PATH"
$triplet = if ($BuildType -eq 'Debug') { 'x64-windows' } else { 'x64-windows-release' }

if ($Phase -ne 'Build') {
    if (-not (Test-Path "$QtPath/lib/cmake/Qt6/Qt6Config.cmake")) {
        throw "Qt $qtVersion MSVC kit not found at $QtPath; pass -QtPath or set QT_ROOT_DIR."
    }
    $versionLine = Select-String -Path "$QtPath/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" `
        -Pattern '^set\(PACKAGE_VERSION "([^"]+)"' | Select-Object -First 1
    if (-not $versionLine -or $versionLine.Matches[0].Groups[1].Value -ne $qtVersion) {
        throw "QtPath must use CI Qt version $qtVersion."
    }
    if (-not (Test-Path "$VcpkgRoot/scripts/buildsystems/vcpkg.cmake")) {
        throw "vcpkg toolchain not found at $VcpkgRoot; set VCPKG_ROOT."
    }
    $expectedVcpkg = if ($env:VCPKG_COMMIT) { $env:VCPKG_COMMIT } else { 'b1c0e26466f37efd4112bf7c1f88f5a8e28c510e' }
    $actualVcpkg = & git -C $VcpkgRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actualVcpkg -ne $expectedVcpkg) {
        throw "VCPKG_ROOT must use CI revision $expectedVcpkg (found $actualVcpkg)."
    }
    if (-not $ProjectedFsLibrary) {
        $sdkLibraries = "${env:ProgramFiles(x86)}\Windows Kits\10\Lib"
        $candidate = Get-ChildItem "$sdkLibraries\*\um\x64\ProjectedFSLib.lib" -ErrorAction SilentlyContinue |
            Sort-Object { [Version]$_.Directory.Parent.Parent.Name } -Descending |
            Select-Object -First 1
        if ($candidate) { $ProjectedFsLibrary = $candidate.FullName }
    }
    if (-not $ProjectedFsLibrary -or -not (Test-Path $ProjectedFsLibrary)) {
        throw 'ProjectedFSLib.lib not found. Install the Windows SDK or pass -ProjectedFsLibrary.'
    }
    $configure = @(
        '-S', $sourceDirectory, '-B', $BuildDirectory,
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_CONFIGURATION_TYPES=$BuildType",
        "-DCMAKE_PREFIX_PATH=$QtPath",
        "-DQT_DIR=$QtPath/lib/cmake/Qt6",
        "-DQt6_DIR=$QtPath/lib/cmake/Qt6",
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_TARGET_TRIPLET=$triplet",
        "-DVCPKG_HOST_TRIPLET=$triplet",
        "-DVCPKG_OVERLAY_TRIPLETS=$sourceDirectory/scripts/vcpkg-triplets",
        '-DVCPKG_MANIFEST_MODE=ON', '-DBUILD_TESTING=ON',
        '-DRAWPREAMP_REQUIRE_VULKAN=ON',
        "-Dprojected-fs=$ProjectedFsLibrary"
    )
    if ($Compiler -eq 'Clang') {
        if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
            $llvmDirectory = "$env:ProgramFiles\LLVM\bin"
            if (Test-Path "$llvmDirectory/clang-cl.exe") { $env:PATH = "$llvmDirectory;$env:PATH" }
            else { throw 'clang-cl not found. Install LLVM and use an x64 Visual Studio developer shell.' }
        }
        $configure += @('-G', 'Ninja Multi-Config', '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
    }
    if ($Phase -eq 'PrintConfig') {
        Write-Output 'cmake'
        $configure | Write-Output
        exit 0
    }
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
}
if ($Phase -eq 'Configure') { exit 0 }

$build = @('--build', $BuildDirectory, '--config', $BuildType,
    '--target', 'RawPreamp', 'rawpreamp-hardware-video-decoder-test', '--parallel')
if ($Parallel -gt 0) { $build += "$Parallel" }
if ($cleanFirst) { $build += '--clean-first' }
& cmake @build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
$executable = Join-Path $BuildDirectory "$BuildType/RawPreamp.exe"
$deployMode = if ($BuildType -eq 'Debug') { '--debug' } else { '--release' }
# Match CI deployment without replacing vcpkg's hardware-enabled FFmpeg DLLs.
& "$QtPath/bin/windeployqt.exe" $deployMode --no-ffmpeg $executable
if ($LASTEXITCODE -ne 0) { throw "Qt deployment failed ($LASTEXITCODE)." }
& ctest --test-dir $BuildDirectory -C $BuildType -R '^rawpreamp-hardware-video-decoder-test$' --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw "Hardware decoder coverage failed ($LASTEXITCODE)." }
Write-Host "Executable: $BuildDirectory/$BuildType/RawPreamp.exe"

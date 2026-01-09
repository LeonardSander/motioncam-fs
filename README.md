# MotionCam Fuse (Fuse-2026)

Desktop tool for mounting MotionCam recordings, exporting DNGs, and reviewing clips.

## Build

Windows (Visual Studio + Qt + vcpkg):
```
git submodule update --init --recursive
.\build.bat
```
Executable lands in `build\Release\MotionCamFuse.exe`.

macOS (Qt + vcpkg, CMake):
```
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Docs

See `HELP.md` for usage, preferences, and feature details.

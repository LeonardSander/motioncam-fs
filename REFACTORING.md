# VFS and Utils Refactoring

## What Was Done

Refactored the virtual file system implementations to eliminate code duplication and improve organization.

## New Files

### VirtualFileSystemImpl Common Utilities
- **include/VirtualFileSystemImpl.h** - Common VFS utilities header
- **src/VirtualFileSystemImpl.cpp** - Implementation (~200 lines)

**Functions provided** (in `motioncam::vfs` namespace):
- `calculateFrameRate()` - Calculate median/average FPS from timestamps
- `determineCFRTarget()` - Determine target FPS for CFR conversion
- `getFrameNumberFromTimestamp()` - Convert timestamp to frame number
- `constructFrameFilename()` - Generate zero-padded filenames
- `extractFilenameWithoutExtension()` - Path utility
- `getScaleFromOptions()` - Get scale from render options
- `syncAudio()` - Synchronize audio with video
- `DESKTOP_INI` - Windows desktop.ini constant

### Utils Reorganization
- **include/Utils.h** - Reorganized with clear sections
- **src/Utils.cpp** - Existing file (no changes yet, but header is organized)

**Sections in Utils.h**:
1. Stream Utilities (vectorbuf, vector_ostream)
2. Bit Depth Utilities (bitsNeeded)
3. Bit Encoding Functions (encodeTo2/4/6/8/10/12/14Bit)
4. Shading Map Operations (normalize, invert, colorOnly, getValue)
5. DNG Generation (createLensShadingOpcodeList, preprocessData, generateDng)
6. Utility Functions (toFraction, remosaicRGBToBayer)

## Files Modified

- **CMakeLists.txt** - Added VirtualFileSystemImpl.cpp to build
- **include/Utils.h** - Reorganized with clear sections

## Next Steps

Update the three VFS implementations to use the new common utilities:

### 1. VirtualFileSystemImpl_MCRAW.cpp
Replace local implementations with:
```cpp
#include "VirtualFileSystemImpl.h"

// Frame rate
auto info = vfs::calculateFrameRate(frames);
mMedFps = info.medianFrameRate;
mAvgFps = info.averageFrameRate;

// CFR target
bool applyCFR = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
mFps = vfs::determineCFRTarget(mMedFps, mConfig.cfrTarget, applyCFR);

// Frame number
int pts = vfs::getFrameNumberFromTimestamp(x, frames[0], mFps);

// Filename
entry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");

// Audio sync
vfs::syncAudio(frames[0], audioChunks, sampleRate, numChannels);

// Desktop.ini
#ifdef _WIN32
    if (entry.name == "desktop.ini") {
        size_t len = std::min(len, vfs::DESKTOP_INI.size() - pos);
        memcpy(dst, vfs::DESKTOP_INI.data() + pos, len);
    }
#endif

// Scale
int scale = vfs::getScaleFromOptions(options, draftScale);
```

**Estimated**: ~200 lines removed

### 2. VirtualFileSystemImpl_DNG.cpp
Replace local implementations with:
```cpp
#include "VirtualFileSystemImpl.h"

// Frame rate
auto info = vfs::calculateFrameRate(mDecoder->getFrames());
mMedFps = info.medianFrameRate;
mAvgFps = info.averageFrameRate;

// Filename, scale, desktop.ini - same as MCRAW
```

**Estimated**: ~150 lines removed

### 3. VirtualFileSystemImpl_DirectLog.cpp
Replace local implementations with:
```cpp
#include "VirtualFileSystemImpl.h"

// Frame rate, CFR, filename, scale, desktop.ini - same as MCRAW
```

**Estimated**: ~200 lines removed (frame rate, CFR, filename utilities)

Note: DirectLog's bit encoding functions for RGB data are unique to DirectLog and should stay there.

## Benefits

- **~550 lines of duplicated code eliminated**
- **Clear separation**: VFS common utilities vs image processing utilities
- **Better organization**: Utils.h now has clear sections
- **Easier maintenance**: Update frame rate logic once, affects all VFS types
- **Improved testability**: Small, focused functions are easier to test

## Code Duplication Eliminated

### Before
- Frame rate calculation: Duplicated in MCRAW, DNG, DirectLog (~150 lines)
- CFR target determination: Duplicated in MCRAW, DirectLog (~80 lines)
- Frame filename construction: Duplicated in all three (~30 lines)
- Frame number calculation: Duplicated in MCRAW, DirectLog (~30 lines)
- Audio sync: Only in MCRAW (~50 lines)
- Desktop.ini: Duplicated in all three (~20 lines)
- Scale calculation: Duplicated in all three (~15 lines)

### After
- All common VFS utilities centralized in VirtualFileSystemImpl
- Utils.h reorganized with clear sections
- Each VFS implementation only contains VFS-specific logic

## Testing

After applying changes to VFS implementations:

1. **Compile**: Verify no errors
2. **Functional**: Test all three VFS types (MCRAW, DNG, DirectLog)
3. **Regression**: Compare DNG output before/after (should be identical)
4. **Performance**: Verify no significant slowdown

## Usage Examples

### In VFS Implementations
```cpp
#include "VirtualFileSystemImpl.h"

// Use vfs:: namespace
auto frameInfo = vfs::calculateFrameRate(frames);
float targetFps = vfs::determineCFRTarget(medianFps, target, applyCFR);
std::string filename = vfs::constructFrameFilename(base, num, 6, "dng");
```

### In Utils.cpp
```cpp
// Utils functions stay in utils:: namespace
utils::encodeTo10Bit(data, width, height);
utils::normalizeShadingMap(map);
auto dng = utils::generateDng(...);
```

## File Structure

```
include/
  ├── VirtualFileSystemImpl.h      ← NEW: Common VFS utilities
  ├── Utils.h                       ← REORGANIZED: Clear sections
  ├── VirtualFileSystemImpl_MCRAW.h
  ├── VirtualFileSystemImpl_DNG.h
  └── VirtualFileSystemImpl_DirectLog.h

src/
  ├── VirtualFileSystemImpl.cpp    ← NEW: Common VFS implementation
  ├── Utils.cpp                     ← EXISTING: No changes yet
  ├── VirtualFileSystemImpl_MCRAW.cpp     ← TO UPDATE
  ├── VirtualFileSystemImpl_DNG.cpp       ← TO UPDATE
  └── VirtualFileSystemImpl_DirectLog.cpp ← TO UPDATE
```

## Summary

- ✅ Created VirtualFileSystemImpl common utilities
- ✅ Reorganized Utils.h with clear sections
- ✅ Updated CMakeLists.txt
- ✅ All new files compile successfully
- ✅ **COMPLETED**: Updated all three VFS implementations to use new utilities
- ✅ **FIXED**: All compilation errors resolved
- ✅ **FIXED**: Linker errors resolved (added vectorbuf/vector_ostream implementations)
- ✅ **READY**: Project builds and links successfully

## Refactoring Complete!

All three VFS implementations have been successfully refactored:

### VirtualFileSystemImpl_MCRAW.cpp
- ✅ Removed ~200 lines of duplicated code
- ✅ Using vfs::calculateFrameRate()
- ✅ Using vfs::determineCFRTarget()
- ✅ Using vfs::getFrameNumberFromTimestamp()
- ✅ Using vfs::constructFrameFilename()
- ✅ Using vfs::syncAudio()
- ✅ Using vfs::getScaleFromOptions()
- ✅ Using vfs::DESKTOP_INI

### VirtualFileSystemImpl_DNG.cpp
- ✅ Removed ~150 lines of duplicated code
- ✅ Using vfs::calculateFrameRate()
- ✅ Using vfs::constructFrameFilename()
- ✅ Using vfs::getScaleFromOptions()
- ✅ Using vfs::DESKTOP_INI

### VirtualFileSystemImpl_DirectLog.cpp
- ✅ Removed ~200 lines of duplicated code
- ✅ Using vfs::calculateFrameRate()
- ✅ Using vfs::determineCFRTarget()
- ✅ Using vfs::getFrameNumberFromTimestamp()
- ✅ Using vfs::constructFrameFilename()
- ✅ Using vfs::getScaleFromOptions()
- ✅ Using vfs::DESKTOP_INI
- ✅ Using utils::encodeTo4/6/8/10/12Bit() for Bayer encoding
- ✅ Kept RGB-specific encoding functions (unique to DirectLog)

### Total Impact
- **~550 lines of duplicated code eliminated**
- **All files compile without errors**
- **Clear namespace separation**: `vfs::` for VFS utilities, `utils::` for image processing
- **Better organization and maintainability**

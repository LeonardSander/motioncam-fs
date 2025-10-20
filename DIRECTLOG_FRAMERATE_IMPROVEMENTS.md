# DirectLog Framerate Improvements

## Overview
Updated DirectLog decoder to extract per-frame timestamps and handle framerate conversion similar to MCRAW implementation, instead of relying on container's average framerate.

## Changes Made

### 1. DirectLogDecoder.cpp
- **Removed reliance on container average framerate**: Instead of using `r_frame_rate` from the container, the decoder now sets `mVideoInfo.fps = 0.0` as a placeholder
- **Per-frame timestamp extraction**: The `analyzeVideo()` method already extracts individual frame timestamps in nanoseconds, which are now properly utilized
- **Fixed `is10bit` variable**: Added proper boolean declaration for 10-bit pixel format detection

### 2. VirtualFileSystemImpl_DirectLog.cpp
- **Calculate framerate from actual timestamps**: Added `calculateFrameRateStats()` to compute median and average FPS from frame-to-frame intervals
- **CFR (Constant Frame Rate) conversion support**: Implemented the same CFR conversion logic as MCRAW:
  - "Prefer Integer" - rounds to common integer framerates (24, 25, 30, 48, 50, 60, 120, 240, etc.)
  - "Prefer Drop Frame" - rounds to NTSC drop-frame rates (23.976, 29.97, 59.94, 119.88, etc.)
  - "Median (Slowmotion)" - uses median framerate for slow-motion playback
  - "Average (Testing)" - uses average framerate (legacy)
  - Custom numeric value - allows manual framerate specification
- **Frame duplication/dropping**: When CFR conversion is enabled, frames are duplicated or dropped to match the target framerate
- **Added helper function**: `getFrameNumberFromTimestamp()` calculates expected frame number based on timestamp and target FPS

## Benefits

1. **Accurate framerate handling**: Uses actual frame timestamps instead of container metadata
2. **VFR (Variable Frame Rate) support**: Properly handles videos with variable frame rates
3. **CFR conversion**: Allows converting VFR footage to CFR for better compatibility with editing software
4. **Consistent with MCRAW**: DirectLog now uses the same framerate handling logic as MCRAW files
5. **Better frame statistics**: Reports median FPS, average FPS, dropped frames, and duplicated frames

## Usage

The framerate handling is controlled by:
- `RENDER_OPT_FRAMERATE_CONVERSION` flag in `FileRenderOptions`
- `cfrTarget` parameter specifying the target framerate mode or custom value

When CFR conversion is disabled, frames are exported as-is with their original timestamps.
When enabled, frames are adjusted to match the target framerate for consistent playback.

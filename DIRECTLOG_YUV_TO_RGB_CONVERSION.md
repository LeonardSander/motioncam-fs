# DirectLog YUV to RGB Conversion Implementation

## Overview
Modified DirectLogDecoder to perform manual YUV to RGB color space conversion using Rec.2020 color space, outputting 16-bit per channel RGB with full data levels.

## Key Changes

### 1. Removed FFmpeg's swscale Library
- Removed `SwsContext` and all related scaling/conversion code
- Removed dependency on `libswscale/swscale.h`
- Removed `mSwsContext`, `mRGBFrame`, `mRGBBuffer`, and `mRGBBufferSize` member variables
- Removed `setupScaler()` method

### 2. Changed Output Format
- **Before:** 8-bit RGB (uint8_t) - 3 bytes per pixel
- **After:** 16-bit RGB (uint16_t) - 6 bytes per pixel
- Output uses full range (0-65535) instead of limited range

### 3. Manual YUV to RGB Conversion

#### Input Assumptions
- YUV data with **limited range**:
  - Y (luma): 16-235 for 8-bit, scaled proportionally for 10-bit
  - U/V (chroma): 16-240 for 8-bit, scaled proportionally for 10-bit
- Supports both 8-bit and 10-bit YUV formats (YUV420P, YUV420P10LE, YUV422P10LE)
- Handles chroma subsampling (4:2:0 and 4:2:2)

#### Conversion Process
1. **Extract YUV values** directly from AVFrame planes
2. **Convert from limited range to normalized [0, 1]**:
   ```
   Y_norm = (Y - 16*scale) / (235*scale - 16*scale)
   U_norm = (U - 16*scale) / (240*scale - 16*scale) - 0.5
   V_norm = (V - 16*scale) / (240*scale - 16*scale) - 0.5
   ```

3. **Apply Rec.2020 YCbCr to RGB matrix**:
   - Kr = 0.2627
   - Kg = 0.6780
   - Kb = 0.0593
   
   ```
   R = Y + 2.0 * (1 - Kr) * V
   G = Y - 2.0 * Kb * (1 - Kb) / Kg * U - 2.0 * Kr * (1 - Kr) / Kg * V
   B = Y + 2.0 * (1 - Kb) * U
   ```

4. **Clamp to [0, 1] and convert to 16-bit full range**:
   ```
   RGB_16bit = clamp(RGB_normalized, 0, 1) * 65535
   ```

5. **Apply HLG to linear conversion** (if video is HLG)

### 4. API Changes

#### DirectLogDecoder.h
```cpp
// Changed from:
bool extractFrame(int frameNumber, std::vector<uint8_t>& rgbData);
bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& rgbData);

// To:
bool extractFrame(int frameNumber, std::vector<uint16_t>& rgbData);
bool extractFrameByTimestamp(Timestamp timestamp, std::vector<uint16_t>& rgbData);
```

#### VirtualFileSystemImpl_DirectLog.h
```cpp
// Changed from:
bool convertRGBToDNG(const std::vector<uint8_t>& rgbData, ...);

// To:
bool convertRGBToDNG(const std::vector<uint16_t>& rgbData, ...);
```

## Benefits

1. **Higher Precision**: 16-bit output preserves more color information
2. **Full Range Output**: Utilizes the complete 0-65535 range for better dynamic range
3. **Accurate Color Space**: Rec.2020 color space properly handles wide color gamut content
4. **No Dependency on swscale**: Direct control over the conversion process
5. **Proper Limited Range Handling**: Correctly interprets limited range YUV input

## Technical Details

### Supported Pixel Formats
- `AV_PIX_FMT_YUV420P` (8-bit, 4:2:0 subsampling)
- `AV_PIX_FMT_YUV420P10LE` (10-bit, 4:2:0 subsampling)
- `AV_PIX_FMT_YUV422P10LE` (10-bit, 4:2:2 subsampling)

### Memory Layout
- Output buffer size: `width * height * 3 * sizeof(uint16_t)` bytes
- Pixel order: R, G, B (interleaved)
- Each channel: 16-bit unsigned integer (0-65535)

## Testing Recommendations

1. Verify color accuracy with known test patterns
2. Compare output with reference implementations
3. Test with both 8-bit and 10-bit source material
4. Validate HLG conversion if applicable
5. Check performance compared to swscale implementation

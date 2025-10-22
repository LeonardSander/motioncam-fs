# DirectLog RGB to Bayer Remosaicing Feature

## Overview
Added support for remosaicing RGB DirectLog output back to Bayer CFA pattern for DNG export. This allows DirectLog videos (which are natively RGB) to be exported as raw Bayer DNGs, which can be useful for certain workflows that expect CFA data.

## Implementation

### 1. CalibrationData Structure Updates
**Files Modified:** `include/CalibrationData.h`, `src/CalibrationData.cpp`

Added one new field to the `CalibrationData` struct:
- `cfaPhase` (string): Specifies the Bayer pattern - "bggr", "rggb", "grbg", or "gbrg"

**Default Behavior:**
- If `cfaPhase` is not specified in JSON, uses the UI setting (default "BGGR")
- Remosaic is controlled via UI checkbox, not JSON

### 2. Remosaicing Function
**Files Modified:** `include/Utils.h`, `src/Utils.cpp`

Added `remosaicRGBToBayer()` function that converts RGB data to Bayer CFA pattern:
```cpp
void remosaicRGBToBayer(const std::vector<uint16_t>& rgbData, 
                        std::vector<uint16_t>& bayerData, 
                        int width, int height, 
                        const std::string& cfaPhase = "bggr");
```

The function:
- Takes RGB interleaved data (R,G,B,R,G,B,...)
- Outputs single-channel Bayer data based on the specified CFA pattern
- Selects the appropriate color channel for each pixel position according to the Bayer pattern

### 3. Bayer Encoding Functions
**File Modified:** `src/VirtualFileSystemImpl_DirectLog.cpp`

Added single-channel encoding functions for Bayer data:
- `encodeTo4Bit()` - Pack to 4-bit
- `encodeTo6Bit()` - Pack to 6-bit
- `encodeTo8Bit()` - Pack to 8-bit
- `encodeTo10Bit()` - Pack to 10-bit
- `encodeTo12Bit()` - Pack to 12-bit

These mirror the RGB encoding functions but work on single-channel data.

### 4. DNG Writing Logic
**File Modified:** `src/VirtualFileSystemImpl_DirectLog.cpp`

Updated `convertRGBToDNG()` to:
- Check if remosaicing is enabled via calibration data
- Convert RGB to Bayer if remosaic flag is true
- Set appropriate DNG metadata:
  - `SamplesPerPixel`: 1 (instead of 3)
  - `Photometric`: 32803 (CFA instead of RGB)
  - `CFAPattern`: Set based on cfaPhase
  - `CFAPlaneColor`: [0, 1, 2] for R, G, B
- Use single-channel encoding functions for bit packing
- Update image description to indicate remosaicing

## Bayer Pattern Mapping

| Pattern | [0,0] | [0,1] | [1,0] | [1,1] |
|---------|-------|-------|-------|-------|
| BGGR    | B     | G     | G     | R     |
| RGGB    | R     | G     | G     | B     |
| GRBG    | G     | R     | B     | G     |
| GBRG    | G     | B     | R     | G     |

## JSON Configuration

### Example Global JSON
Updated `CalibrationData::createExampleJson()` to include:
```json
{
  "_comment4": "For DirectLog RGB remosaic - Bayer phases rggb grbg gbrg bggr (default bggr if not specified)",
  "_cfaPhase": "bggr"
}
```

### Per-File JSON Example
To override CFA phase for a specific DirectLog video, create a JSON file with the same name:
```
video.MOV
video.json  <- calibration file
```

**video.json (CFA phase override only):**
```json
{
  "cfaPhase": "rggb"
}
```

**video.json (with other calibration data):**
```json
{
  "colorMatrix1": [0.7643, -0.2137, -0.0822, -0.5013, 1.3478, 0.1644, -0.1315, 0.1972, 0.5588],
  "asShotNeutral": [0.5, 1.0, 0.5],
  "cfaPhase": "rggb"
}
```

## Usage

### UI Controls
1. **Enable Remosaicing:**
   - Check "Remosaic RGB to Bayer CFA (DirectLog)" in the Preprocessing section
   - Select CFA phase from dropdown:
     - "Don't override CFA" (default) - uses BGGR unless overridden by JSON
     - BGGR, RGGB, GRBG, or GBRG - forces specific pattern

2. **Per-File Override:**
   - Create a `.json` file next to your DirectLog video to override CFA phase
   - Example: `video.json` with `{"cfaPhase": "rggb"}`
   - The JSON phase always takes priority over UI setting

3. **Without remosaicing (default):**
   - Leave checkbox unchecked
   - DirectLog videos export as RGB DNGs

## Technical Notes

- Remosaicing is a lossy operation - it discards 2/3 of the color information at each pixel
- The resulting Bayer DNG will require demosaicing in post-processing software
- This feature is primarily useful for workflows that expect raw Bayer data
- All log encoding and exposure compensation features work with remosaiced output
- The CFA pattern metadata is properly set in the DNG for correct demosaicing

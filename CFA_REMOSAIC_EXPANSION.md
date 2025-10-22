# CFA Pattern and Remosaic Expansion

## Overview
Expanding remosaic and CFA pattern functionality to handle RGB inputs from all sources (DirectLog, MCRAW, DNG) and provide CFA phase correction for misinterpreted Bayer patterns. This is purely about data interpretation - no pixel reordering occurs.

## Key Concepts

### Data Interpretation vs Pixel Reordering
- **CFA Phase**: Tells the system how to interpret the color channel arrangement in Bayer data
- **No Pixel Reordering**: Pixels stay in their original positions; only the interpretation changes
- **Remosaic**: Discards interpolated pixel values from demosaiced RGB data to recover original Bayer pattern

### Use Cases

#### 1. RGB Input Remosaicing
When input files contain RGB data (either natively or after demosaic):
- DirectLog videos (natively RGB/YUV)
- MCRAW files that were demosaiced during capture
- DNG sequences that contain RGB data (PhotometricInterpretation = RGB)

The remosaic function converts RGB back to single-channel Bayer by selecting the appropriate color channel for each pixel position.

#### 2. CFA Phase Correction
When Bayer data has incorrect CFA pattern metadata:
- MCRAW files with wrong sensorArrangement in metadata
- DNG files with incorrect CFAPattern tag
- Any Bayer input where the pattern was misidentified during capture

The cfaPhase parameter overrides the metadata to correctly interpret the existing Bayer data.

## Implementation Strategy

### 1. Input Detection
Each VFS implementation needs to detect whether input is RGB or Bayer:

**DirectLog (VirtualFileSystemImpl_DirectLog.cpp)**
- Already RGB (YUV converted to RGB)
- Always requires remosaic for Bayer output

**MCRAW (VirtualFileSystemImpl_MCRAW.cpp)**
- Check metadata: `sensorArrangement` field
- If empty or "RGB": Input is RGB, remosaic available
- If "RGGB", "BGGR", etc.: Input is Bayer, only CFA phase correction available

**DNG (VirtualFileSystemImpl_DNG.cpp)**
- Read DNG tags: `PhotometricInterpretation` and `SamplesPerPixel`
- If PhotometricInterpretation = RGB (34892) or SamplesPerPixel = 3: Input is RGB
- If PhotometricInterpretation = CFA (32803) and SamplesPerPixel = 1: Input is Bayer

### 2. CFA Phase Handling

#### Priority Order
1. **UI/API cfaPhase parameter** (if not "Don't override CFA")
2. **Per-file JSON calibration** (`cfaPhase` field)
3. **Input file metadata** (sensorArrangement, CFAPattern)
4. **Default fallback** ("bggr")

#### For RGB Inputs
- cfaPhase determines target Bayer pattern for remosaic
- Used by `remosaicRGBToBayer()` function
- Sets CFAPattern in output DNG

#### For Bayer Inputs
- cfaPhase overrides metadata interpretation
- No remosaic occurs (data already single-channel)
- Updates CFAPattern in output DNG to match corrected interpretation

### 3. Remosaic Function Enhancement

Current function (already implemented):
```cpp
void remosaicRGBToBayer(
    const std::vector<uint16_t>& rgbData,
    std::vector<uint16_t>& bayerData,
    int width, int height,
    const std::string& cfaPhase = "bggr");
```

This function:
- Takes RGB interleaved data (R,G,B,R,G,B,...)
- Outputs single-channel Bayer based on cfaPhase
- Selects appropriate color channel per pixel position
- Discards 2/3 of color information (the interpolated values)

### 4. CFA Phase Correction Function

New function needed for Bayer inputs:
```cpp
void applyCfaPhaseCorrection(
    std::vector<uint8_t>& dngData,
    const std::string& originalPhase,
    const std::string& correctedPhase);
```

This function:
- Modifies DNG metadata only (CFAPattern tag)
- Does NOT reorder pixels
- Updates interpretation so demosaic software reads colors correctly

## CalibrationData Updates

No new fields needed - `cfaPhase` already exists:
```cpp
struct CalibrationData {
    // ... existing fields ...
    std::string cfaPhase;  // "bggr", "rggb", "grbg", "gbrg"
};
```

Usage:
- For RGB inputs: Target pattern for remosaic
- For Bayer inputs: Corrected pattern interpretation
- Can be set in per-file JSON or global calibration JSON

## VFS Implementation Changes

### VirtualFileSystemImpl_DirectLog
**Current State**: Already supports remosaic and cfaPhase
**Changes**: None needed (already complete)

### VirtualFileSystemImpl_MCRAW
**Changes Needed**:
1. Detect if input is RGB or Bayer from metadata
2. If RGB: Enable remosaic option, use cfaPhase for target pattern
3. If Bayer: Apply cfaPhase as correction to metadata interpretation
4. Store cfaPhase parameter (currently ignored)

**Key Code Locations**:
- Constructor: Accept and store cfaPhase parameter
- `updateOptions()`: Store cfaPhase (currently has comment "only used for DirectLog")
- `generateFrame()`: Check input type and apply remosaic or phase correction

### VirtualFileSystemImpl_DNG
**Changes Needed**:
1. Detect if input DNGs are RGB or Bayer
2. If RGB: Enable remosaic option, use cfaPhase for target pattern
3. If Bayer: Apply cfaPhase as correction to CFAPattern tag
4. Store cfaPhase parameter (currently ignored)

**Key Code Locations**:
- Constructor: Accept and store cfaPhase parameter
- `updateOptions()`: Store cfaPhase (currently has comment "only used for DirectLog")
- `generateFrame()`: Check input type and apply remosaic or phase correction
- DNGDecoder: May need to expose PhotometricInterpretation info

## UI/API Considerations

### Remosaic Checkbox
- Label: "Remosaic RGB to Bayer CFA"
- Enabled: Only when input is detected as RGB
- Disabled: When input is already Bayer (show tooltip: "Input is already Bayer")

### CFA Phase Dropdown
- Options: "Don't override CFA", "BGGR", "RGGB", "GRBG", "GBRG"
- For RGB inputs: Determines target Bayer pattern
- For Bayer inputs: Corrects pattern interpretation
- Always available (works for both RGB and Bayer)

### Status Display
Show detected input type:
- "Input: RGB (remosaic available)"
- "Input: Bayer RGGB (phase correction available)"
- "Input: Bayer (unknown pattern)"

## Example Workflows

### Workflow 1: DirectLog RGB to Bayer
```
Input: DirectLog MOV (RGB/YUV)
Remosaic: Enabled
CFA Phase: RGGB
Output: Single-channel Bayer DNG with RGGB pattern
```

### Workflow 2: MCRAW with Wrong CFA Metadata
```
Input: MCRAW (Bayer RGGB, but metadata says BGGR)
Remosaic: N/A (already Bayer)
CFA Phase: RGGB (corrects metadata)
Output: Bayer DNG with corrected RGGB pattern tag
```

### Workflow 3: RGB DNG Sequence to Bayer
```
Input: DNG sequence (PhotometricInterpretation=RGB)
Remosaic: Enabled
CFA Phase: GRBG
Output: Single-channel Bayer DNGs with GRBG pattern
```

### Workflow 4: MCRAW RGB to Bayer
```
Input: MCRAW (demosaiced RGB, sensorArrangement empty)
Remosaic: Enabled
CFA Phase: BGGR
Output: Single-channel Bayer DNG with BGGR pattern
```

## Technical Notes

### Remosaic is Lossy
- Discards 2/3 of color information at each pixel
- Only keeps the "native" color channel for each Bayer position
- Resulting DNG requires demosaicing in post software

### CFA Phase Correction is Lossless
- Only changes metadata interpretation
- No pixel data modified
- Fixes color channel misidentification

### Pattern Validation
All implementations validate cfaPhase:
- Accepted: "bggr", "rggb", "grbg", "gbrg" (case-insensitive)
- Invalid values default to "bggr"
- Empty string uses metadata or default

### Metadata Priority
1. Explicit UI/API parameter (if not "Don't override")
2. Per-file JSON calibration
3. Input file metadata
4. Default "bggr"

## Testing Checklist

### RGB Input Tests
- [ ] DirectLog remosaic with each CFA phase
- [ ] MCRAW RGB detection and remosaic
- [ ] DNG RGB detection and remosaic
- [ ] Per-file JSON cfaPhase override
- [ ] UI cfaPhase selection

### Bayer Input Tests
- [ ] MCRAW Bayer detection (no remosaic)
- [ ] DNG Bayer detection (no remosaic)
- [ ] CFA phase correction for each pattern
- [ ] Metadata override priority
- [ ] Output DNG CFAPattern tag correctness

### Edge Cases
- [ ] Missing metadata (use defaults)
- [ ] Invalid cfaPhase values (fallback to bggr)
- [ ] Mixed RGB/Bayer in same sequence
- [ ] Empty sensorArrangement field
- [ ] Quad Bayer patterns (future consideration)

## Future Enhancements

### Quad Bayer Support
- 4x4 patterns for quad Bayer sensors
- Requires 16-element CFA pattern
- May need separate remosaic function

### Auto-Detection
- Analyze pixel data to detect likely CFA pattern
- Compare against metadata
- Suggest corrections to user

### Pattern Visualization
- Show visual representation of CFA pattern
- Highlight which channels are kept during remosaic
- Preview before/after demosaic results

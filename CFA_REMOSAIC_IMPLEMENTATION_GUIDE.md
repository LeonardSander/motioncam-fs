# CFA Remosaic Implementation Guide

## Code Changes Required

### 1. VirtualFileSystemImpl_MCRAW Changes

#### Header (include/VirtualFileSystemImpl_MCRAW.h)
```cpp
// Add member variable
private:
    std::string mCfaPhase;  // Add this line
    bool mInputIsRGB;       // Track if input is RGB or Bayer
```

#### Implementation (src/VirtualFileSystemImpl_MCRAW.cpp)

**Constructor - Store cfaPhase parameter:**
```cpp
VirtualFileSystemImpl_MCRAW::VirtualFileSystemImpl_MCRAW(
    // ... parameters ...
    const std::string& quadBayerOption,
    const std::string& cfaPhase) :  // Already exists
    // ... initialization ...
    mQuadBayerOption(quadBayerOption),
    mCfaPhase(cfaPhase),  // ADD THIS
    mInputIsRGB(false),   // ADD THIS
    mOptions(options) {
    
    init(options);
}
```

**init() - Detect RGB vs Bayer:**
```cpp
void VirtualFileSystemImpl_MCRAW::init(FileRenderOptions options) {
    // ... existing code ...
    
    // After loading metadata, detect input type
    auto decoder = Decoder::open(mSrcPath);
    if (decoder) {
        auto metadata = decoder->getCameraMetadata();
        std::string sensorArrangement = metadata.sensorArrangement;
        
        // Convert to lowercase for comparison
        std::transform(sensorArrangement.begin(), sensorArrangement.end(), 
                      sensorArrangement.begin(), ::tolower);
        
        // Check if RGB or empty (demosaiced)
        if (sensorArrangement.empty() || sensorArrangement == "rgb") {
            mInputIsRGB = true;
            spdlog::info("MCRAW input detected as RGB (remosaic available)");
        } else {
            mInputIsRGB = false;
            spdlog::info("MCRAW input detected as Bayer: {}", sensorArrangement);
        }
    }
}
```

**updateOptions() - Store cfaPhase:**
```cpp
void VirtualFileSystemImpl_MCRAW::updateOptions(
    // ... parameters ...
    const std::string& cfaPhase) {
    
    // ... existing code ...
    mQuadBayerOption = quadBayerOption;
    mCfaPhase = cfaPhase;  // CHANGE: Remove "only for DirectLog" comment
    
    // ... rest of function ...
}
```

**generateFrame() - Apply remosaic or phase correction:**
```cpp
size_t VirtualFileSystemImpl_MCRAW::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    
    // ... existing frame extraction code ...
    
    // After getting frame data, check if remosaic needed
    if (mInputIsRGB && (mOptions & RENDER_OPT_REMOSAIC)) {
        // Input is RGB and remosaic is enabled
        std::vector<uint16_t> rgbData;
        // Extract RGB data from frame
        // ... (implementation depends on existing frame format)
        
        // Determine CFA phase
        std::string cfaPhase = mCfaPhase;
        if (cfaPhase.empty() || cfaPhase == "Don't override CFA") {
            // Use from calibration or default
            if (mCalibration && !mCalibration->cfaPhase.empty()) {
                cfaPhase = mCalibration->cfaPhase;
            } else {
                cfaPhase = "bggr";
            }
        }
        
        // Remosaic RGB to Bayer
        std::vector<uint16_t> bayerData;
        utils::remosaicRGBToBayer(rgbData, bayerData, mWidth, mHeight, cfaPhase);
        
        // Convert to DNG with Bayer data
        // ... (use existing DNG writing code, set CFA metadata)
        
    } else if (!mInputIsRGB && !mCfaPhase.empty() && mCfaPhase != "Don't override CFA") {
        // Input is Bayer and CFA phase correction requested
        // Modify DNG metadata to correct CFAPattern
        // ... (update CFAPattern tag in DNG)
        
    } else {
        // Pass through existing data
        // ... (existing code path)
    }
    
    // ... rest of function ...
}
```

### 2. VirtualFileSystemImpl_DNG Changes

#### Header (include/VirtualFileSystemImpl_DNG.h)
```cpp
// Add member variables
private:
    std::string mCfaPhase;  // Add this line
    bool mInputIsRGB;       // Track if input is RGB or Bayer
```

#### Implementation (src/VirtualFileSystemImpl_DNG.cpp)

**Constructor - Store cfaPhase parameter:**
```cpp
VirtualFileSystemImpl_DNG::VirtualFileSystemImpl_DNG(
    // ... parameters ...
    const std::string& quadBayerOption,
    const std::string& cfaPhase) :  // Already exists
    // ... initialization ...
    mQuadBayerOption(quadBayerOption),
    mCfaPhase(cfaPhase),  // ADD THIS
    mInputIsRGB(false),   // ADD THIS
    mOptions(options) {
    
    init(options);
}
```

**init() - Detect RGB vs Bayer:**
```cpp
void VirtualFileSystemImpl_DNG::init(FileRenderOptions options) {
    // ... existing code ...
    
    mDecoder = std::make_unique<DNGDecoder>(mSrcPath);
    
    // Detect if first DNG is RGB or Bayer
    if (!mDecoder->getFrames().empty()) {
        std::vector<uint8_t> firstDngData;
        if (mDecoder->extractFrame(0, firstDngData)) {
            // Parse DNG to check PhotometricInterpretation
            // This requires adding a helper function to DNGDecoder
            mInputIsRGB = DNGDecoder::isRGBDNG(firstDngData);
            
            if (mInputIsRGB) {
                spdlog::info("DNG input detected as RGB (remosaic available)");
            } else {
                spdlog::info("DNG input detected as Bayer");
            }
        }
    }
    
    // ... rest of init ...
}
```

**updateOptions() - Store cfaPhase:**
```cpp
void VirtualFileSystemImpl_DNG::updateOptions(
    // ... parameters ...
    const std::string& cfaPhase) {
    
    // ... existing code ...
    mQuadBayerOption = quadBayerOption;
    mCfaPhase = cfaPhase;  // CHANGE: Remove "only for DirectLog" comment
    
    // ... rest of function ...
}
```

**generateFrame() - Apply remosaic or phase correction:**
```cpp
size_t VirtualFileSystemImpl_DNG::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    
    // ... existing DNG extraction code ...
    
    if (mInputIsRGB && (mOptions & RENDER_OPT_REMOSAIC)) {
        // Input DNG is RGB and remosaic is enabled
        // Extract RGB data from DNG
        std::vector<uint16_t> rgbData;
        // ... (parse DNG to get RGB pixel data)
        
        // Determine CFA phase
        std::string cfaPhase = mCfaPhase;
        if (cfaPhase.empty() || cfaPhase == "Don't override CFA") {
            if (mCalibration && !mCalibration->cfaPhase.empty()) {
                cfaPhase = mCalibration->cfaPhase;
            } else {
                cfaPhase = "bggr";
            }
        }
        
        // Remosaic RGB to Bayer
        std::vector<uint16_t> bayerData;
        utils::remosaicRGBToBayer(rgbData, bayerData, mWidth, mHeight, cfaPhase);
        
        // Rewrite DNG with Bayer data and correct metadata
        // ... (create new DNG with CFA metadata)
        
    } else if (!mInputIsRGB && !mCfaPhase.empty() && mCfaPhase != "Don't override CFA") {
        // Input is Bayer and CFA phase correction requested
        // Modify DNG CFAPattern tag
        // ... (update metadata only)
        
    } else {
        // Pass through existing DNG
        // ... (existing code path)
    }
    
    // ... rest of function ...
}
```

### 3. DNGDecoder Helper Function

#### Header (include/DNGDecoder.h)
```cpp
class DNGDecoder {
public:
    // ... existing methods ...
    
    // Add static helper to detect RGB vs Bayer DNG
    static bool isRGBDNG(const std::vector<uint8_t>& dngData);
    static bool isRGBDNG(const std::string& dngPath);
};
```

#### Implementation (src/DNGDecoder.cpp)
```cpp
bool DNGDecoder::isRGBDNG(const std::vector<uint8_t>& dngData) {
    // Parse DNG TIFF structure to find PhotometricInterpretation tag
    // Tag 262 (0x106): PhotometricInterpretation
    //   Value 32803 (0x8023) = CFA (Bayer)
    //   Value 34892 (0x884C) = LinearRaw (RGB)
    //   Value 2 = RGB
    // Tag 277 (0x115): SamplesPerPixel
    //   Value 1 = Single channel (Bayer)
    //   Value 3 = RGB
    
    // Simple heuristic: Check SamplesPerPixel
    // (Full implementation would parse TIFF IFD structure)
    
    // For now, return false (assume Bayer) - needs proper implementation
    return false;
}

bool DNGDecoder::isRGBDNG(const std::string& dngPath) {
    std::vector<uint8_t> dngData;
    if (readDNGFile(dngPath, dngData)) {
        return isRGBDNG(dngData);
    }
    return false;
}
```

### 4. Utils.h CFA Phase Correction Function

#### Header (include/Utils.h)
```cpp
namespace utils {

// Existing function
void remosaicRGBToBayer(
    const std::vector<uint16_t>& rgbData,
    std::vector<uint16_t>& bayerData,
    int width, int height,
    const std::string& cfaPhase = "bggr");

// NEW: Apply CFA phase correction to DNG metadata
bool applyCfaPhaseCorrection(
    std::vector<uint8_t>& dngData,
    const std::string& correctedPhase);

} // namespace utils
```

#### Implementation (src/Utils.cpp)
```cpp
bool applyCfaPhaseCorrection(
    std::vector<uint8_t>& dngData,
    const std::string& correctedPhase) {
    
    // Validate phase
    std::string phase = correctedPhase;
    std::transform(phase.begin(), phase.end(), phase.begin(), ::tolower);
    if (phase != "bggr" && phase != "rggb" && phase != "grbg" && phase != "gbrg") {
        return false;
    }
    
    // Parse DNG TIFF structure to find CFAPattern tag (33422 / 0x828E)
    // Update the 4-byte pattern array based on correctedPhase
    // This requires TIFF IFD parsing and modification
    
    // Map phase to CFA pattern bytes
    unsigned char cfaPattern[4];
    if (phase == "bggr") {
        cfaPattern[0] = 2; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 0;
    } else if (phase == "rggb") {
        cfaPattern[0] = 0; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 2;
    } else if (phase == "grbg") {
        cfaPattern[0] = 1; cfaPattern[1] = 0; cfaPattern[2] = 2; cfaPattern[3] = 1;
    } else { // gbrg
        cfaPattern[0] = 1; cfaPattern[1] = 2; cfaPattern[2] = 0; cfaPattern[3] = 1;
    }
    
    // Find and update CFAPattern tag in DNG data
    // (Implementation requires TIFF parsing - complex)
    
    return true;
}
```

### 5. IVirtualFileSystem Interface

#### Header (include/IVirtualFileSystem.h)
```cpp
// Add flag for remosaic option
enum FileRenderOptions {
    // ... existing flags ...
    RENDER_OPT_REMOSAIC = 1 << 6,  // ADD THIS
};
```

## Summary of Changes

### Files to Modify
1. `include/VirtualFileSystemImpl_MCRAW.h` - Add mCfaPhase, mInputIsRGB
2. `src/VirtualFileSystemImpl_MCRAW.cpp` - Implement RGB detection and remosaic
3. `include/VirtualFileSystemImpl_DNG.h` - Add mCfaPhase, mInputIsRGB
4. `src/VirtualFileSystemImpl_DNG.cpp` - Implement RGB detection and remosaic
5. `include/DNGDecoder.h` - Add isRGBDNG() helper
6. `src/DNGDecoder.cpp` - Implement isRGBDNG()
7. `include/Utils.h` - Add applyCfaPhaseCorrection()
8. `src/Utils.cpp` - Implement applyCfaPhaseCorrection()
9. `include/IVirtualFileSystem.h` - Add RENDER_OPT_REMOSAIC flag

### Key Implementation Points

1. **RGB Detection**: Check metadata/tags to determine if input is RGB or Bayer
2. **CFA Phase Priority**: UI > JSON > Metadata > Default
3. **Remosaic Path**: Only for RGB inputs when enabled
4. **Phase Correction Path**: Only for Bayer inputs when cfaPhase specified
5. **No Pixel Reordering**: All operations preserve pixel positions

### Testing Strategy

1. Test with DirectLog (already working)
2. Test with MCRAW RGB inputs
3. Test with MCRAW Bayer inputs + phase correction
4. Test with DNG RGB sequences
5. Test with DNG Bayer sequences + phase correction
6. Verify metadata priority order
7. Verify output DNG CFAPattern tags

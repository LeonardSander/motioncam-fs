# Render Configuration Consolidation

## Overview
This refactoring consolidates all UI file render options and other configuration parameters that were previously passed through many function calls across multiple classes into a single `RenderConfig` structure.

## Problem
Previously, render options were passed as 10+ individual parameters through multiple layers:
- MainWindow → IFuseFileSystem → Session → IVirtualFileSystem implementations
- Parameters included: `options`, `draftScale`, `cfrTarget`, `cropTarget`, `cameraModel`, `levels`, `logTransform`, `exposureCompensation`, `quadBayerOption`, `cfaPhase`

This created:
- Long, unwieldy function signatures
- Difficult maintenance when adding new options
- Error-prone parameter passing
- Code duplication

## Solution
Created a new `RenderConfig` structure in `include/RenderConfig.h` that encapsulates all rendering parameters:

```cpp
struct RenderConfig {
    FileRenderOptions options;
    int draftScale;
    std::string cfrTarget;
    std::string cropTarget;
    std::string cameraModel;
    std::string levels;
    std::string logTransform;
    std::string exposureCompensation;
    std::string quadBayerOption;
    std::string cfaPhase;
};
```

## Changes Made

### 1. New Files
- `include/RenderConfig.h` - Consolidated configuration structure

### 2. Updated Interfaces
- `include/IVirtualFileSystem.h` - Changed `updateOptions()` to accept `const RenderConfig&`
- `include/IFuseFileSystem.h` - Changed `mount()` and `updateOptions()` to use `RenderConfig`

### 3. Updated VFS Implementations
- `include/VirtualFileSystemImpl_DirectLog.h` - Simplified constructor and methods
- `include/VirtualFileSystemImpl_MCRAW.h` - Simplified constructor and methods
- `include/VirtualFileSystemImpl_DNG.h` - Simplified constructor and methods
- All implementations now store a single `mConfig` member instead of 10+ individual members

### 4. Updated Platform Implementations
- `include/win/FuseFileSystemImpl_Win.h` - Updated signatures
- `src/win/FuseFileSystemImpl_Win.cpp` - Updated Session class and mount/update methods
- `include/macos/FuseFileSystemImpl_MacOS.h` - Updated signatures
- `src/macos/FuseFileSystemImpl_MacOS.cpp` - Updated Session class and mount/update methods

### 5. Updated MainWindow
- `include/mainwindow.h` - Added `buildRenderConfig()` method, replaced individual members with `mRenderConfig`
- `src/mainwindow.cpp` - Removed `getRenderOptions()` helper, consolidated all option handling

## Benefits

1. **Cleaner Function Signatures**: Functions now accept a single `RenderConfig` parameter instead of 10+ individual parameters
2. **Easier Maintenance**: Adding new options only requires updating the `RenderConfig` structure
3. **Better Encapsulation**: Related configuration data is grouped together
4. **Reduced Code Duplication**: Configuration is built once and passed by reference
5. **Type Safety**: Configuration is a well-defined structure with default values
6. **Improved Readability**: Code intent is clearer with named structure members

## Migration Guide

### Before
```cpp
mount(options, draftScale, cfrTarget, cropTarget, cameraModel, 
      levels, logTransform, exposureCompensation, quadBayerOption, 
      cfaPhase, srcFile, dstPath);
```

### After
```cpp
RenderConfig config;
config.options = options;
config.draftScale = draftScale;
// ... set other fields
mount(config, srcFile, dstPath);
```

## Backward Compatibility
The `RenderConfig` structure includes a constructor that accepts all parameters for easier migration, though the preferred approach is to use the default constructor and set fields individually.

## Future Improvements
- Consider adding validation methods to `RenderConfig`
- Add serialization/deserialization for saving/loading configurations
- Create preset configurations for common use cases

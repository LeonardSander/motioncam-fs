# Calibration JSON Sidecar Implementation

## Overview
This implementation adds support for per-file calibration JSON sidecar files that can override color matrices and as-shot neutral values in DNG output.

## Features

### 1. Global Calibration Template
- On first launch, a `calibration.json` example file is created in the application directory
- This file serves as a template with example values and comments explaining the format
- If the file already exists, it's loaded and used as the default for creating per-file calibrations

### 2. Per-File Calibration JSON
- Each video file (MCRAW, MP4/MOV, or DNG sequence) can have a sidecar JSON file
- The JSON file should be in the same directory with the same base name
  - Example: `video.mcraw` → `video.json`
  - Example: `NATIVE_001.mp4` → `NATIVE_001.json`

### 3. UI Integration
- **"Create JSON" Button**: Appears on the right side of each file entry in the UI
- When clicked, creates a calibration JSON file using the global template
- After JSON is created, the button is replaced with a status label:
  - **"Calibration Loaded"** (green) - JSON parsed successfully
  - **"Calibration Ignored"** (red) - JSON exists but failed to parse
- Status is checked on UI updates and when files are mounted

### 4. Supported Calibration Fields

All fields are optional. Fields prefixed with `_` are ignored (disabled by default in template):

```json
{
  "colorMatrix1": [0.7643, -0.2137, -0.0822, -0.5013, 1.3478, 0.1644, -0.1315, 0.1972, 0.5588],
  "colorMatrix2": [0.9329, -0.3914, -0.0326, -0.5806, 1.4092, 0.1827, -0.0913, 0.1761, 0.5872],
  "forwardMatrix1": [0.6484, 0.2734, 0.0469, 0.2344, 0.8984, -0.1328, 0.0469, -0.1797, 0.9609],
  "forwardMatrix2": [0.6875, 0.1563, 0.125, 0.2734, 0.7578, -0.0313, 0.0859, -0.4688, 1.2109],
  "asShotNeutral": [0.5, 1.0, 0.5]
}
```

**Note:** The default template prefixes all fields with `_` to disable them. Simply remove the underscore to enable a field.

### 5. Flexible Matrix Format
Matrix values can be specified in two ways:
- **Array format**: `[val1, val2, val3, ...]`
- **Space-separated string**: `"val1 val2 val3 ..."`

This allows for easier manual editing without commas.

### 6. Application Priority
When generating DNGs, calibration data is applied with this priority:
1. Per-file calibration JSON (if exists and valid)
2. Original camera metadata from the source file

Only the fields present in the calibration JSON override the defaults.

## File Structure

### New Files
- `include/CalibrationData.h` - Calibration data structure and parsing
- `src/CalibrationData.cpp` - Implementation of JSON parsing and loading

### Modified Files
- `include/mainwindow.h` - Added calibration button handlers
- `src/mainwindow.cpp` - UI integration for calibration buttons
- `include/Utils.h` - Added calibration parameter to generateDng
- `src/Utils.cpp` - Apply calibration data when creating DNGs
- `include/VirtualFileSystemImpl_*.h` - Added calibration member
- `src/VirtualFileSystemImpl_*.cpp` - Load and pass calibration data
- `CMakeLists.txt` - Added CalibrationData source files

## Usage Workflow

1. **Mount a video file** (MCRAW, MP4/MOV, or DNG sequence)
2. **Click "Create JSON"** button on the file entry
3. **Edit the JSON file** in the source directory with your calibration values
4. **Remount the file** or wait for UI update to see "Calibration Loaded" status
5. **Generated DNGs** will now use the calibration data

## Technical Details

### Calibration Loading
- Calibration is loaded once when a file is mounted
- JSON is searched in the same directory as the source file
- Parsing errors are logged but don't prevent mounting

### DNG Generation
- Calibration data is passed through the entire pipeline
- Applied in `Utils.cpp::generateDng()` function
- Only overrides specified fields, preserves others from camera metadata

### Supported File Types
- **MCRAW files** (.mcraw) - Full support for all calibration fields
- **DirectLog videos** (.mp4/.mov with NATIVE) - Full support for all calibration fields (RGB DNGs)
- **DNG sequences** - Full support for all calibration fields

## Example Calibration JSON

The default template has all fields disabled (prefixed with `_`). Remove the underscore to enable a field:

```json
{
  "_comment": "Calibration data for DNG color processing",
  "_comment2": "Matrix values can be separated by comma or space",
  "_comment3": "So far only these fields can be overriden. Remove _ in _colorMatrix1 to enable override.",
  "_colorMatrix1": [0.7643, -0.2137, -0.0822, -0.5013, 1.3478, 0.1644, -0.1315, 0.1972, 0.5588],
  "_colorMatrix2": [0.9329, -0.3914, -0.0326, -0.5806, 1.4092, 0.1827, -0.0913, 0.1761, 0.5872],
  "_forwardMatrix1": [0.6484, 0.2734, 0.0469, 0.2344, 0.8984, -0.1328, 0.0469, -0.1797, 0.9609],
  "_forwardMatrix2": [0.6875, 0.1563, 0.125, 0.2734, 0.7578, -0.0313, 0.0859, -0.4688, 1.2109],
  "_asShotNeutral": [0.5, 1.0, 0.5]
}
```

To enable a field, simply remove the underscore prefix:
```json
{
  "colorMatrix1": [0.7643, -0.2137, -0.0822, -0.5013, 1.3478, 0.1644, -0.1315, 0.1972, 0.5588]
}
```

## Notes

- Comment fields (starting with `_`) are ignored during parsing
- Invalid JSON files will show "Calibration Ignored" status
- The global `calibration.json` in the app directory is used as a template
- Calibration data is applied per-frame during DNG generation

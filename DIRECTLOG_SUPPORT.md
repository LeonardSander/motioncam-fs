# DirectLog Support for MOV/MP4 Files and DNG Sequences

## Overview

MotionCam Fuse now supports both MOV/MP4 files with NATIVE suffix and DNG sequences in addition to MCRAW files. These inputs contain raw image data that has been processed through linearization, demosaic, and optionally HLG transfer curve treatment.

## Supported Input Types

### 1. Video Files (MOV/MP4)
**File Naming Convention:**
- Files must end with `NATIVE.mov` or `NATIVE.mp4`
- For HLG videos: `*HLG_NATIVE.mov` or `*HLG_NATIVE.mp4`
- For linear videos: `*LINEAR_NATIVE.mov` or `*LINEAR_NATIVE.mp4`

**Video Specifications:**
- **Pixel Formats**: yuv420p, yuv420p10le, yuv422p10le
- **Color Space**: Rec2020 (assumed for underlying image data and only concerns the pixel format conversion from yuv to rgb)
- **Processing**: Only linearization, demosaic, and optional HLG curve applied
- **No Processing**: No white balance or color space transforms applied

### 2. DNG Sequences
**File Structure:**
- Directory containing sequential DNG files
- Individual DNG files with frame numbers in filename
- Supports opcode gain maps for vignette correction

**DNG Specifications:**
- **Format**: Standard Adobe DNG format
- **Gain Maps**: Reads opcode lists 2 and 3 for vignette correction
- **Timestamps**: Extracted from filename patterns or file metadata
- **Processing**: Direct DNG passthrough with optional vignette correction

## Processing Pipeline

1. **Video Input**: MOV/MP4 file with raw image data in YUV format
2. **Frame Extraction**: Using FFmpeg to extract individual frames
3. **YUV to RGB Conversion**: Convert from YUV to RGB color space
4. **HLG Linearization**: If HLG curve was applied, convert back to linear
5. **DNG Output**: Generate DNG files with either:
   - Mosaiced raw data (re-applying Bayer pattern)
   - Linear RGB data (demosaiced)
6. **Log Encoding**: Apply log encoding similar to MCRAW processing

## Implementation Details

### New Classes
- `VirtualFileSystemImpl_DirectLog`: Handles MOV/MP4 NATIVE files
- `DirectLogDecoder`: FFmpeg-based decoder for video frame extraction
- Inherits from `IVirtualFileSystem` interface
- Similar structure to `VirtualFileSystemImpl_MCRAW`

### Key Features
- **FFmpeg Integration**: Full FFmpeg library integration for video processing
- **Real-time Frame Extraction**: Extracts video frames on-demand using FFmpeg
- **YUV to RGB Conversion**: Proper color space conversion using libswscale
- **HLG to Linear**: Implements HLG transfer curve to linear conversion
- **No Vignette Correction**: Gainmaps are ignored since no vignette correction data is available
- **Empty DNG Tags**: AsShot neutral and color matrices are left empty (not available in video files)
- **10-bit Assumption**: For "Keep Input Log" option, assumes 10-bit bit depth

### File Structure
When mounted, DirectLog files create a virtual directory structure with:
- Individual DNG files for each frame (e.g., `basename000001.dng`)
- Desktop.ini file (Windows only)
- Same naming convention as MCRAW files

## Usage

1. **Drag and Drop**: Simply drag MOV/MP4 files with NATIVE suffix into MotionCam Fuse
2. **Processing Options**: All standard processing options apply (draft mode, cropping, etc.)
3. **Output**: Access individual DNG frames through the mounted virtual directory

## Technical Notes

- **DirectLogDecoder Class**: Complete FFmpeg integration for video processing
- **Variable Frame Rate**: Reads and preserves original PTS timestamps from video
- **Frame Extraction**: Real-time seeking and decoding using FFmpeg APIs
- **Color Space Conversion**: Efficient YUV to RGB conversion using libswscale
- **HLG Processing**: Implements HLG to linear transfer curve conversion
- **Memory Management**: Proper FFmpeg resource management and cleanup
- **Timestamp Compatibility**: Uses same nanosecond timestamp format as MCRAW

## Current Implementation Status

**FFmpeg Integration (Working Now)**:
- **Real Video Processing**: Full FFmpeg integration for MOV/MP4 files
- **Variable Frame Rate Support**: Preserves original PTS timestamps like MCRAW
- **Frame Extraction**: Real-time frame seeking and RGB extraction
- **Color Space Conversion**: YUV to RGB using libswscale
- **HLG Processing**: Automatic HLG to linear conversion
- **Frame Rate Statistics**: Calculates median, average, and target FPS
- **Timestamp Preservation**: Maintains original video timing without CFR conversion

**Core Features**:
- File detection and mounting of MOV/MP4 NATIVE files
- HLG vs LINEAR detection from filename
- Proper virtual filesystem integration
- DNG file structure creation with real frame data

## Future Enhancements

**Advanced Features**:
- **DNG Generation**: Complete RGB to DNG conversion with proper metadata
- **Bayer Re-mosaicing**: Convert RGB back to raw Bayer pattern for raw DNG output
- **Performance Optimizations**: Caching and multi-threaded frame extraction
- **Extended Format Support**: Support for additional pixel formats and codecs
- **Metadata Preservation**: Extract and preserve video metadata in DNG files

## FFmpeg Integration

The implementation now includes full FFmpeg support:

### Key Features:
- **Variable Frame Rate**: Preserves original video timing without constant frame rate conversion
- **PTS Preservation**: Maintains presentation timestamps for accurate frame timing
- **Real-time Processing**: Extracts and converts video frames on-demand
- **Format Support**: Handles yuv420p, yuv420p10le, yuv422p10le pixel formats
- **HLG Support**: Automatic detection and conversion of HLG transfer curves

### Frame Processing Pipeline:
1. **Video Analysis**: Reads all frame PTS values to build timestamp map
2. **Frame Seeking**: Uses PTS for accurate frame positioning
3. **YUV Decoding**: Extracts raw YUV data from video stream
4. **Color Conversion**: Converts YUV to RGB using libswscale
5. **HLG Processing**: Applies HLG to linear conversion if detected
6. **DNG Generation**: Creates virtual DNG files with processed frame data

This approach mirrors the MCRAW workflow where variable frame timing is preserved and processed without forcing constant frame rates.
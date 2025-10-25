# Precache Feature Implementation

## Overview
Added a precache feature that automatically renders and caches all frames of mounted clips in the background. The system intelligently prioritizes user-accessed frames and resumes precaching from the last accessed position.

## Key Features

### 1. Background Precaching
- Renders and caches frames sequentially per clip
- Processes clips in the order they were mounted
- Frames within each clip are rendered sequentially
- Automatically cycles through all mounted clips

### 2. User Access Priority
- When a user accesses a frame (via file read), precaching immediately pauses
- The accessed frame is prioritized and rendered immediately
- After 1 second of no file access, precaching resumes from the last accessed frame in the corresponding clip
- This ensures smooth user experience without blocking

### 3. UI Integration
- **Settings Panel**: Added "Enable Precaching" checkbox at the bottom of the settings section
- **Clip Metrics**: Each clip now shows cache progress as a percentage
  - Green (100%): Clip fully cached
  - Orange (X%): Currently caching this clip
  - Gray (0%): Not yet cached

### 4. Frame Rate Conversion Support
- Precaching respects the constant frame rate (CFR) conversion settings
- Iterates over the correct number of frames after potential CFR conversion
- Ensures all frames that will be accessed are properly cached

## Implementation Details

### UI Changes
**File**: `ui/mainwindow.ui`
- Added `precacheCheckBox` in the settings section
- Added descriptive label explaining the feature

### MainWindow Class
**Files**: `include/mainwindow.h`, `src/mainwindow.cpp`

#### New Member Variables
- `mPrecacheEnabled`: Boolean flag for precache state
- `mPrecacheTimer`: Timer that triggers frame precaching (10ms interval)
- `mPrecacheResumeTimer`: Single-shot timer for resuming after user access (1 second)
- `mCurrentPrecacheClipIndex`: Tracks which clip is being precached
- `mCurrentPrecacheFrameIndex`: Tracks which frame within the clip
- `mLastAccessedMountId`: Stores the last accessed clip
- `mLastAccessedFrameIndex`: Stores the last accessed frame number
- `mPrecacheActive`: Atomic flag indicating if precaching is active
- `mPrecacheWatcher`: Future watcher for async precaching tasks

#### New Methods
- `startPrecaching()`: Initializes precaching from the first clip
- `stopPrecaching()`: Stops all precaching timers
- `precacheNextFrame()`: Processes the next frame in the queue
- `onFrameAccessed()`: Handles user frame access events
- `updateCacheProgress()`: Updates the UI with cache progress percentages
- `onPrecacheCheckboxChanged()`: Handles checkbox state changes

### Virtual File System Interface
**File**: `include/IFuseFileSystem.h`

Added two new methods to support precaching:
- `listFiles(MountId mountId)`: Returns all file entries for a mount
- `readFile(MountId mountId, const Entry& entry, size_t pos, size_t len, void* dst)`: Synchronously reads a file to trigger caching

### Platform Implementations

#### Windows (ProjFS)
**Files**: `include/win/FuseFileSystemImpl_Win.h`, `src/win/FuseFileSystemImpl_Win.cpp`
- Implemented `listFiles()` and `readFile()` in both `FuseFileSystemImpl_Win` and `Session` classes
- Uses synchronous read with atomic completion tracking

#### macOS (FUSE)
**Files**: `include/macos/FuseFileSystemImpl_MacOS.h`, `src/macos/FuseFileSystemImpl_MacOS.cpp`
- Implemented `listFiles()` and `readFile()` in both `FuseFileSystemImpl_MacOs` and `Session` classes
- Uses synchronous read with atomic completion tracking

## Usage

1. **Enable Precaching**: Check the "Enable Precaching" checkbox in the Settings panel
2. **Mount Clips**: Drag and drop clips as usual
3. **Monitor Progress**: Watch the cache percentage in each clip's metrics
4. **Access Frames**: Open frames normally - precaching will pause and prioritize your access
5. **Automatic Resume**: After 1 second of inactivity, precaching continues from where you left off

## Technical Notes

### Caching Strategy
- Uses the existing LRU cache infrastructure
- Each frame read triggers the full rendering pipeline
- Cached frames are stored in memory up to the cache size limit
- Cache is shared across all mounted clips

### Performance Considerations
- Precaching runs on background threads (QtConcurrent)
- Does not block the UI or user interactions
- Automatically throttles when user accesses frames
- Progress updates every 10 frames to minimize UI overhead

### Settings Persistence
- Precache enabled/disabled state is saved in QSettings
- Restored on application restart
- Per-user setting (not per-project)

## Future Enhancements

Potential improvements for future versions:
1. Cache statistics display (total cached size, cache hit rate)
2. Configurable precache priority (sequential, random, smart prediction)
3. Disk-based cache overflow for very large projects
4. Per-clip precache enable/disable
5. Precache speed control (frames per second limit)

# UI Threading Improvements

## Problem
The UI was freezing whenever render settings were changed because image processing operations (`updateOptions`) were running on the main UI thread, blocking user interaction.

## Solution
Implemented asynchronous processing with taskbar progress feedback:

### Key Changes

1. **Worker Thread**: Created a dedicated `QThread` for processing operations
   - Prevents UI blocking during heavy image processing
   - Reuses the same thread for multiple operations

2. **Windows Taskbar Progress**: Uses native Windows taskbar progress indicator
   - Shows progress directly on the taskbar icon
   - Automatically hides when complete
   - Non-intrusive visual feedback
   - Platform-specific (Windows only)

3. **Async Processing**: `scheduleOptionsUpdate()` method
   - Captures current settings
   - Offloads `updateOptions()` calls to worker thread
   - Updates progress for each mounted file processed
   - Prevents multiple simultaneous processing operations

### Modified Files

- `include/mainwindow.h`: Added thread management and Windows taskbar progress members
- `src/mainwindow.cpp`: Implemented async processing logic

### How It Works

1. User changes a UI option (checkbox, dropdown, etc.)
2. `scheduleOptionsUpdate()` is called instead of blocking immediately
3. Taskbar icon shows progress bar (Windows only)
4. Processing happens on worker thread
5. Progress updates as each file is processed
6. UI remains responsive throughout
7. FPS labels update when processing completes

### Benefits

- **Responsive UI**: Users can continue interacting with the interface
- **Native Progress**: Uses Windows taskbar progress for clean, OS-integrated feedback
- **Better UX**: No more frozen application during processing
- **Thread Safety**: Proper Qt signal/slot mechanism for cross-thread communication
- **Non-intrusive**: Progress shown on taskbar, doesn't take up UI space

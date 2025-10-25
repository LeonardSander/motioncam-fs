when comparing the experimental branch to origin a lot of prototype unfinished functionality was implemented on top of an already convoluted codebase. especially now much functionality between mcraw direct log and dng input is duplicate. maybe there is a way to structure all that in a more efficient and understandable manner. another thing that bothers me is how ui options are passed through a dozen function calls throughout several cpps. however the underlying functionality and uiux are exactly as intended and are not to be changed.




# Requirements Document

## Introduction

This document defines requirements for refactoring the MCRAW Player codebase to eliminate duplicate functionality across MCRAW, DirectLog, and DNG implementations, and to simplify the parameter passing architecture for UI options. The refactoring will improve code maintainability and understandability while preserving all existing functionality and user experience.

## Glossary

- **VirtualFileSystem**: The abstraction layer that handles reading and processing video frames from different source formats (MCRAW, DirectLog, DNG)
- **RenderOptions**: Configuration parameters that control how frames are processed and rendered (draft quality, CFR target, crop settings, color transforms, etc.)
- **MainWindow**: The Qt-based UI component that collects user settings and manages mounted files
- **FuseFileSystem**: Platform-specific filesystem implementation (Windows/macOS) that manages virtual file mounts
- **Session**: Platform-specific wrapper around VirtualFileSystem instances within FuseFileSystem
- **Decoder**: Format-specific component that handles the actual decoding of video frames (DirectLogDecoder, DNGDecoder, or MCRAW Decoder)

## Requirements

### Requirement 1

**User Story:** As a developer maintaining the codebase, I want common functionality consolidated into shared base classes, so that bug fixes and enhancements only need to be implemented once.

#### Acceptance Criteria

1. WHEN examining the VirtualFileSystem implementations, THE System SHALL contain a base class that implements all shared functionality across MCRAW, DirectLog, and DNG formats
2. THE System SHALL delegate only format-specific decoding logic to derived classes
3. THE System SHALL eliminate duplicate implementations of frame rate calculation, file listing, cache management, and option updates
4. THE System SHALL maintain identical behavior for all existing functionality after refactoring

### Requirement 2

**User Story:** As a developer modifying render options, I want UI settings encapsulated in a single configuration object, so that I don't need to modify dozens of function signatures when adding new options.

#### Acceptance Criteria

1. THE System SHALL encapsulate all render configuration parameters in a single RenderConfig structure
2. WHEN a new render option is added, THE System SHALL require modifications to only the RenderConfig structure and its usage points
3. THE System SHALL replace all multi-parameter function signatures with single RenderConfig parameter passing
4. THE System SHALL maintain backward compatibility with existing option values and defaults

### Requirement 3

**User Story:** As a developer debugging the codebase, I want clear separation between UI concerns and processing logic, so that I can understand the data flow without tracing through multiple layers.

#### Acceptance Criteria

1. THE System SHALL limit render option parameter passing to a maximum of three layers (UI → FileSystem → VirtualFileSystem)
2. THE System SHALL eliminate intermediate parameter forwarding through Session classes
3. THE System SHALL use direct configuration object passing instead of parameter unpacking and repacking
4. THE System SHALL maintain clear ownership and lifecycle of configuration objects

### Requirement 4

**User Story:** As a developer working with the codebase, I want consistent patterns across all format implementations, so that knowledge gained from one implementation applies to others.

#### Acceptance Criteria

1. THE System SHALL use identical member variable names and types across all VirtualFileSystem implementations
2. THE System SHALL use consistent initialization patterns in all VirtualFileSystem constructors
3. THE System SHALL use uniform error handling approaches across all implementations
4. THE System SHALL document format-specific differences explicitly in code comments

### Requirement 5

**User Story:** As a developer extending the system with new formats, I want a clear template for implementation, so that I can add new formats with minimal effort.

#### Acceptance Criteria

1. THE System SHALL provide a base VirtualFileSystem class with protected helper methods for common operations
2. THE System SHALL require derived classes to implement only format-specific decoding methods
3. THE System SHALL document the minimal interface required for new format implementations
4. THE System SHALL provide example implementations showing the pattern for both simple (DNG) and complex (MCRAW with audio) formats

### Requirement 6

**User Story:** As a user of the application, I want all existing features to work identically after refactoring, so that my workflow is not disrupted.

#### Acceptance Criteria

1. THE System SHALL preserve all existing UI options and their behaviors
2. THE System SHALL maintain identical frame processing output for all formats
3. THE System SHALL preserve all performance characteristics including caching and threading
4. THE System SHALL maintain compatibility with all existing MCRAW, DirectLog, and DNG files

### Requirement 7

**User Story:** As a developer optimizing the codebase, I want minimalistic implementations without unnecessary variables and functions, so that the code remains lean and performant.

#### Acceptance Criteria

1. THE System SHALL eliminate all unused member variables from VirtualFileSystem implementations
2. THE System SHALL remove all redundant helper functions that duplicate existing functionality
3. THE System SHALL store only essential state required for frame processing
4. THE System SHALL avoid premature optimization or speculative features

### Requirement 8

**User Story:** As a user processing DirectLog files, I want frame generation to be fast and responsive, so that I can work efficiently with large video files.

#### Acceptance Criteria

1. WHEN processing DirectLog frames, THE System SHALL minimize memory allocations per frame
2. THE System SHALL eliminate redundant data copies in the DirectLog processing pipeline
3. THE System SHALL optimize DirectLog decoder performance to match or exceed MCRAW processing speed
4. THE System SHALL profile and identify DirectLog performance bottlenecks during refactoring
5. THE System SHALL implement efficient caching strategies specific to DirectLog format characteristics

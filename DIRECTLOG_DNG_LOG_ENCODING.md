# DirectLog DNG Log Encoding Implementation

## Overview
This document describes the implementation of log curve encoding for DirectLog DNG exports, which reduces 48-bit RGB (16-bit per channel) to 12-bit per channel with optional additional bit reduction.

## Problem Statement
DirectLog videos are decoded to 48-bit RGB (16-bit per channel). When exporting to DNG, we want to:
1. Always apply a log curve to reduce to 12-bit per channel when the log option is enabled
2. Apply additional bit reduction (2, 4, 6, or 8 bits) if specified
3. Include a linearization table in the DNG to reverse the log curve

## Implementation Details

### Log Curve Application
The implementation applies a logarithmic tone mapping curve to compress the dynamic range:

```
logValue = log2(1 + 60*x) / log2(61)
```

Where:
- `x` is the normalized input value [0, 1]
- The constant 60 (k=60) matches the MCRAW implementation
- This preserves black (0) and white (1) as identity points

### Bit Depth Reduction
1. **Initial reduction to 12-bit**: When log transform is enabled, the 16-bit RGB data is first reduced to 12-bit (4095 levels) using the log curve
2. **Additional reduction**: If "Reduce by X bits" is specified, further reduce from 12-bit:
   - "Reduce by 2bit": 12-bit → 10-bit (1023 levels)
   - "Reduce by 4bit": 12-bit → 8-bit (255 levels)
   - "Reduce by 6bit": 12-bit → 6-bit (63 levels)
   - "Reduce by 8bit": 12-bit → 4-bit (15 levels)

### Linearization Table
A linearization table is added to the DNG to reverse the log curve:

```
linearValue = (2^(logValue * log2(61)) - 1) / 60
```

This allows DNG processors to correctly interpret the log-encoded data as linear values.

### Code Changes

#### Modified Function: `VirtualFileSystemImpl_DirectLog::convertRGBToDNG`

**Key changes:**
1. Parse `mLogTransform` to determine if log encoding should be applied
2. Extract bit reduction amount from the log transform option
3. Apply log curve to reduce 16-bit RGB to 12-bit per channel
4. Apply additional bit reduction if specified
5. Create and set linearization table in the DNG
6. Set appropriate white/black levels (0 and 65534 for log-encoded data)

## Usage

The log encoding is controlled by the `logTransform` parameter:
- Empty string or not set: No log encoding (original 16-bit data)
- "Keep Input": Apply log curve to 12-bit, no additional reduction
- "Reduce by 2bit": Apply log curve to 12-bit, then reduce to 10-bit
- "Reduce by 4bit": Apply log curve to 12-bit, then reduce to 8-bit
- "Reduce by 6bit": Apply log curve to 12-bit, then reduce to 6-bit
- "Reduce by 8bit": Apply log curve to 12-bit, then reduce to 4-bit

## Benefits

1. **Reduced file size**: 12-bit encoding reduces DNG file size compared to 16-bit
2. **Better dynamic range utilization**: Log curve allocates more bits to shadows
3. **Compatibility**: Linearization table ensures correct interpretation by DNG processors
4. **Consistency**: Matches the MCRAW implementation for uniform behavior

## Technical Notes

- The log curve uses k=60, which provides a good balance between shadow detail and highlight preservation
- Black (0) and white (maximum value) are preserved as identity points
- The linearization table has (dstWhiteLevel + 1) entries to cover the full range
- White level is set to 65534 (not 65535) to match MCRAW implementation

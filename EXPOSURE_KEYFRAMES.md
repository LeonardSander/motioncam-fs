# Exposure Keyframes Feature

## Overview

The exposure normalization system now supports keyframe-based interpolation for smooth exposure transitions across your DNG sequence. This allows you to create gradual exposure changes without visible steps or fractures.

## Usage

In the **Normalize Exposure** combobox, you can enter either:

### Constant Value (Original Behavior)
- Simple EV offset: `2ev`, `-1.5ev`, `0ev`
- Applied uniformly to all frames

### Keyframe Syntax (New Feature)
Define keyframes as comma-separated position:value pairs:

```
0.2:-4, 0.4:2.4
```

Or use `start` and `end` synonyms:

```
start:-2, 0.5:0, end:2
```

## Keyframe Position

Positions are **normalized** (0.0 to 1.0) relative to the total frame count:
- `0` or `start` = first frame (index 0)
- `1` or `end` = last frame (index frameCount - 1)
- `0.2` = frame at index `frameCount * 0.2`
- `0.5` = middle frame

**Note:** Positions reference the frame count **after** constant framerate conversion (CFR), matching the actual DNG output sequence.

## Interpolation Behavior

### Smooth Transitions
The system uses **Cubic Hermite spline interpolation** for smooth, fracture-free transitions between keyframes.

### Constant Regions
- Frames **before** the first keyframe use the first keyframe's value
- Frames **after** the last keyframe use the last keyframe's value
- This ensures constant exposure outside transition zones

### Derivative Rules
The derivative (slope) at each keyframe is automatically calculated:
- **Zero derivative** at most keyframes (default for smooth curves)
- **Non-zero derivative** allowed only at:
  - Position 0 or 1 (start/end)
  - Monotonic points where values are strictly increasing (prev < current < next)
  - Monotonic points where values are strictly decreasing (prev > current > next)
- **Zero derivative** enforced at:
  - Local extrema (peaks and valleys) to ensure smooth transitions
  - Any keyframe that doesn't meet the above criteria

This ensures fracture-free, smooth transitions with natural curvature.

## Examples

### Example 1: Fade from dark to bright
```
start:-4, end:0
```
Smoothly transitions from -4 EV at the beginning to 0 EV at the end.

### Example 2: Exposure dip in the middle
```
0:-2, 0.5:-4, 1:-2
```
Starts at -2 EV, dips to -4 EV at the midpoint, then returns to -2 EV.

### Example 3: Complex transition
```
0.2:-4, 0.4:2.4, 0.8:1
```
- Frames 0 to 20% of sequence: constant -4 EV
- Frames 20% to 40%: smooth transition from -4 EV to +2.4 EV
- Frames 40% to 80%: smooth transition from +2.4 EV to +1 EV
- Frames 80% to 100%: constant +1 EV

### Example 4: Sunrise effect
```
start:-3, 0.3:-1, 0.6:1, end:2
```
Gradually increases exposure to simulate a sunrise.

## Technical Details

### Frame Index Calculation
For a sequence with `N` frames (after CFR conversion):
- Position `p` maps to frame index: `floor(p * (N - 1))`
- Frame 0 is at position 0.0
- Frame N-1 is at position 1.0

### Spline Interpolation
Uses Cubic Hermite interpolation formula:
```
h(t) = (2t³ - 3t² + 1)p₀ + (t³ - 2t² + t)m₀ + (-2t³ + 3t²)p₁ + (t³ - t²)m₁
```

Where:
- `t` = normalized position within segment [0, 1]
- `p₀`, `p₁` = keyframe values
- `m₀`, `m₁` = derivatives (tangents) at keyframes

### Derivative Calculation
- At extrema (peaks/valleys): derivative = 0 (smooth curve)
- At monotonic points: derivative = average of adjacent slopes
- At boundaries (0 or 1): derivative = slope to adjacent keyframe

## Integration

The keyframe system is automatically detected when you enter comma-separated values. If parsing fails, the system falls back to treating the input as a constant value.

Keyframes are re-parsed whenever you:
- Change the exposure compensation value
- Remount a file
- Update render settings

## Logging

Check the application logs for keyframe parsing information:
```
[info] Parsed 3 exposure keyframes
[debug]   Keyframe: pos=0.000, value=-4.00ev, deriv=0.00
[debug]   Keyframe: pos=0.400, value=2.40ev, deriv=16.00
[debug]   Keyframe: pos=1.000, value=2.00ev, deriv=-0.67
```

## Compatibility

- Works with both MCRAW and DirectLog video sources
- Compatible with all other render options (CFR, cropping, vignette correction, etc.)
- Frame-specific exposure values are calculated on-demand during DNG generation

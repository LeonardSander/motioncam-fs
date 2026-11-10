# LOG60 and LLOG60

When log encoding is enabled, uncompressed DNG, lossless JPEG DNG and
lossless JPEG XL DNG use LOG60. Lossy JPEG DNG and JPEG XL DCT DNG use
LLOG60. All Camera Native video encoders use LLOG60.

LLOG60 preserves normalized black = 0 and white = 1:

- Through linear 0.96: LOG60 multiplied by 0.980913053208808.
- From 0.96 to 0.99: quadratic transition with continuous value and slope.
- From 0.99 to 1: identity, with slope 1 (about ten full-range 10-bit codes).

The transition starts at encoded 0.971335276138720. The curve and its inverse
are in `include/RawLogTransform.h`. Matching GPU arithmetic is in
`shaders/vulkan_gain_display_math.glsl` and `src/DirectLogGpuRgb.cpp`.

DNG output passes an explicit `LogCurve` through standalone and fused CPU
and Vulkan encoders. The same selection builds the embedded TIFF
LinearizationTable. Both encoder lookup caches include the curve in their
key. Import reads the embedded table; compression alone does not determine
the inverse, so existing lossy LOG60 DNGs continue to decode correctly.
Disabling log encoding still leaves the existing linear-output behavior.

Camera Native output filenames contain `_LLOG60_NATIVE`, and their sidecars
identify the transfer as `LLOG60`. Keep the filename marker: video import
uses it to select the CPU/GPU inverse. Original `_LOG60_NATIVE` clips retain
LOG60 decoding. Experimental `_LOG60_NATIVE_S1` clips retain LLOG60 decoding.
Older builds do not recognize the new inverse and are unsuitable for import.

The modified curve reduces amplification of encoded errors near clipping.
It does not ensure that lossy codecs preserve an exact clipped plateau;
white-level tolerance may still be needed before highlight reconstruction.

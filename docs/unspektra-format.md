# Unspektra sample format (v2 / UFR1)

These observations were validated against `Unspektrawesome-1791657397501.unspektra`
and the public Unspektrawesome 1.2.3 APK. `tools/unspektra_extract.py` extracts
one indexed frame to a little-endian 16-bit Bayer raster plus its recipe JSON.

* The 20-byte file header begins with `UNSPKTRA`, a big-endian 32-bit version
  (`2`), and a big-endian 64-bit index offset.
* Records have a 12-byte big-endian header: payload size, type (`0` in this
  sample), CRC32 of the payload. The final record is a JSON index containing
  frame recipe/raw offsets, timestamps, capture time, display dimensions, and
  thumbnail offset. Recipes are zlib-compressed JSON. The thumbnail is JPEG.
* A raw record contains `UFR1`, followed by little-endian 32-bit raster width,
  height, and CFA size. The observed samples use `2` (Bayer), `4` (Quad Bayer),
  `6`, and `8`. They have a 4096 × 3072 raster and 3072 × 4096
  recipe display dimensions. The recipe's `raw` blob stores half this CFA size
  at byte offset `0x40`.
* The UFR1 stream is a sequence of 16 × 16 tiles in row-major order. Each tile
  contains four 8 × 8 groups, ordered by `(y parity, x parity)` as `(0,0)`,
  `(0,1)`, `(1,0)`, `(1,1)`. Each group has one byte of bit width (0–16), a
  little-endian 16-bit reference, then 64 little-endian bit-packed unsigned
  deltas. The raster sample is `reference + delta`. Groups at right/bottom
  edges retain all 64 encoded values even when some lie outside the raster.

The extractor checks record CRCs, block bounds, and complete UFR1 consumption.
The first frame decodes to a coherent image with samples from 43 to 15408.
This documents the observed 2 × 2, 4 × 4, 6 × 6, and 8 × 8 CFA forms; future versions need
separate validation before enabling them in MotionCam Fuse.

MotionCam Fuse accepts `.unspektra` through its MCRAW mounting pipeline. The
sample's recipe supplies black/white levels, exposure, ISO and white balance.
Its `shading` blob contains four interleaved 17 × 13 float gain maps; the last
two floats in the `color` blob give the grid dimensions. Fuse can bake these
maps or retain them as DNG GainMap opcodes.

The 144-byte `color` blob is nine little-endian `vec4` values. Inspection of
the app's embedded `RawColorParameters` shader identifies offsets `0x00` as
white-balance/exposure gains, `0x10`–`0x3f` as three padded columns of a
camera-to-working RGB matrix, and `0x80` as geometry controls whose last two
floats give the shading-grid width and height. Other controls are app rendering
parameters. The app's shader optionally applies a DCP table after this matrix,
but the table offset at `0x60` is zero in both clips, so that step is bypassed.
This 3 × 3 matrix is the only explicit color transform found in the recipes.
Its relationship to DNG ColorMatrix, CameraCalibration, and ForwardMatrix tags
has not been established, so Fuse does not write it as one of those tags.
Per-frame recipe metadata also supplies Make, Model, lens name, aperture,
physical focal length, and 35 mm equivalent focal length for DNG/EXIF tags.

The raw descriptor stores the CFA phase twice, at offsets `0x18` and `0x44`.
The values are the red pixel's two-bit `(x, y)` position in the color group:
`0` RGGB, `1` GRBG, `2` GBRG, `3` BGGR. The phase-overridden 4 × 4 sample has
`3` in both fields; the other samples have `0`. Fuse uses the recorded phase
when producing DNG CFA tags. The byte at `0x1c` varies between clips but is
not needed to identify the phase. A calibration sidecar can supply DNG color
matrices for a specific camera.

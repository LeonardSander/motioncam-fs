# mc24 CPU decoder

`mc24` is the internal/display name for **mcpro24fps `.mcpro`** recordings.
The container implementation lives in `deps/motioncam-decoder/lib/Mc24.cpp`;
`Mc24Raw.cpp` contains the strip decoder and CRC validation helpers. Both use
`motioncam::Decoder`, the same interface used by MCRAW and Unspektra.
Recognition uses the `MCPH` signature; file routing uses `.mcpro`.

## Container

The decoder scans records by their declared lengths, without reading compressed
frame payloads during indexing. It validates record boundaries. CRC32C validation is disabled by default;
set `RAWPREAMP_MC24_VALIDATE_CHECKSUMS=1` to validate record headers and
compressed strips. Checksum fields and both hardware/portable validation
implementations are retained.

- `MCPH`: recording/camera metadata. Camera matrices retain signed rational
  values and illuminants.
- `MCRF`: compressed raster. The decoder reconstructs stored sample codes as RAW16.
- `MCRL`: linearization lookup table referenced by each frame. Linear/log bit depth reduction and HQ/SQ/LT
  modes use this mechanism. The decoder returns stored codes plus the per-frame
  `linearizationTable` metadata. Canonical DNG staging writes that table into the
  DNG LinearizationTable tag; the shared DNG pipeline applies it when needed.
  Keeping the input preserves both codes and table, just like an imported DNG.
- `MCPM`: per-frame metadata. Complete records supply all tags; delta
  records inherit base tags and replace only those present in the delta.
- `MCPB` kind 1: lens shading maps referenced by frame metadata.
- `MCPA`: PCM audio, including timestamps, rate, channels and sample format.
- `MCPG`: timestamped gyro and accelerometer samples, returned through the
  existing decoder motion APIs without axis conversion.

Index, trailer, recovery and thumbnail records do not supply raster pixels.

## Frame codec

The strip decoder reads packed residuals and reconstructs Bayer samples using
only the predictor and packing parameters stored in the input. Prediction state
restarts for each strip. It returns stored sample codes without applying the
linearization table. No MC24 encoder, residual packer, predictor selection,
quantization policy, or container writer is included.

The supported scope is version 1 Bayer recordings using coder 0, packing modes
0/1 (including a mode-3 strip split), predictors 0/1, explicit MCRL tables,
float32 or int16 PCM audio, and version-1 32-byte motion samples. Unsupported
codec variants fail explicitly. No converter binary or runtime is needed.

## Validation

Test captures: `/home/leonards/Videos/mcpro24fps/mcpro test captures/`, recorded
by mcpro24fps 044 on a OnePlus CPH2581, 4096×3072, approximately 60 fps.

- Seven unquantized lossless captures report the same effective 10-bit sensor
  range despite the requested recording-depth changes.
- Two lossless captures use 8-bit linear and 8-bit log reconstruction tables.
- The last three captures use HQ, SQ and LT reconstruction tables.
- The CPU strip decoder was compared sample for sample against the CPU strip
  routine in the official RAW Converter 0.4.0 for all 1,942 frames in all 12 files.
  Every compressed strip also passed its CRC32C check.
- Decoder integration checks loaded the first, middle and last frame of every
  capture, and extracted PCM audio, gyro and acceleration samples.
- The release app imported all 12 clips in an isolated offscreen session.
  Gallery playback and seeking succeeded for the 8-bit log and HQ captures.
- `decoder_mc24_test` / `rawpreamp-mc24-test` cover metadata inheritance, random
  frame access, Bayer prediction, lane packing, table application, audio,
  motion, truncated input and corrupted strip rejection. Existing decoder,
  gyro, acceleration, core, audio-finalization and interpolation tests passed.

Reference binary used only during format analysis:
[official mcpro24fps RAW Converter](https://mcpro24fps.com/downloads/raw-converter/mcpro24fpsRawConverter-0.5.1-x64.exe).

The clip UI uses `MC24LL`, `MC24H`, `MC24S`, or `MC24L` for lossless, HQ,
SQ, and LT. Source bit depth and the recording log flag are kept separately
from post-linearization sensor levels.

## Timing and metadata coverage

Frame timestamps are retained as nanoseconds from each MCRF record. With CFR
conversion disabled, preview and DNG/video output retain the source intervals,
subtracting the first timestamp to start at zero. Explicit CFR conversion
resamples the timeline. The supplied files already use an approximately
16.6635417 ms timestamp grid, with gaps spanning multiple slots; this alignment
is present in the container and is not introduced by Fuse.

Metadata export is selective: decoded camera/color/lens/exposure/level/noise
fields, shading and linearization tables, audio, and motion are supported.
Arbitrary proprietary tags and auxiliary recording records are not all copied
to exported DNGs. Keep the original container for complete original metadata.

## CPU performance

On the supplied 4096×3072 captures, median frame loading across the 12 clips
improved from approximately 84 ms to 28 ms with checksum validation enabled
for both measurements. Each clip benchmark loads nine successive frames,
discards the first timing, and reports the median of the remaining eight;
these are warm-cache CPU measurements, not cold-storage throughput.

Block-width-specialized unpacking removes per-sample byte-gather loops and
runtime lane shifts. Prediction separates seeded rows and edge samples from
the interior loop. Frames decode directly into the caller's reusable RAW16
buffer, avoiding an extra full-frame allocation and copy. Row reconstruction is
separated into seeded and adaptive Bayer helpers; residual storage stays separate
from output to preserve compiler optimization. The scalar adaptive predictor
reuses its left/top bounds for both range testing and gradient clipping.
CRC32C has runtime
SSE4.2 dispatch on supported x86 CPUs and a portable slicing-by-eight fallback.
Validation remains optional as described above.

Verification includes all 1,942 capture frames against the reference decoder,
2,304 randomized strips against the original implementation during development,
fixed regression vectors covering all block widths and both packing layouts,
and both CRC paths against a bitwise reference. Regression inputs are stored
as fixed bytes; the tests do not construct or encode MC24 recordings.

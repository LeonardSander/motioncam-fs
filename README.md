
# MotionCam Fuse – Virtual File System

> **Ai-assisted work in progress**

**MotionCam Fuse** allows opening raw image and video files, such as MCRAW and [Unspektra](https://github.com/EkinStrop/Unspektrawesome-Releases). These are mounted as projected folders containing DNG sequences. Several processing and calibration options aim to provide an optimal raw editing workflow—preferably in **Davinci Resolve** for video. Clips can also be inspected in an integrated Gallery, allowing for playback and specific frame selections. To save processed files to disk as intermediates or for archival, mounted clips may be finalized with many video and DNG compression options.

--- 
 
<img width="2621" height="1396" alt="ui" src="https://github.com/user-attachments/assets/3fed84d2-77e2-4380-80f3-2e7443df2947" />

---

When clips are mounted, folders containing respective frames will appear in their source directory if not defined otherwise under Settings > Preferences. As soon as the individual frames are accessed, their image contents are processed and cached.

For Windows builds the output directory needs to be on a NTFS drive. Also on Windows cached files are written into the projected folder structure and may remain along empty projected files when Fuse is closed irregularly or if Discard on Unmount is disabled under Preferences. These files will be properly accessible again when Fuse is opened again and the session is resumed. The cache can be manually cleared by closing Fuse, deleting folder contents first and folders afterwards.

For now nightly builds are available under GitHub Actions Artifacts. 

---

### Features

[Showcase video](https://youtu.be/knACG5jy-rk)

- **Constant frame rate conversion**

To account for non-standard or variable frame rates (VFR), a conversion to a suitable standard constant frame rate (CFR) is implemented. Based on the target frame rate, derived from a clip's median frame rate, captured frames are dynamically duplicated or dropped. This conversion ensures real time playback avoiding desync in a CFR editing timeline like Davinci Resolve. For non-real time playback a clip's median frame rate can be chosen as target as well. The per frame timing deviation from timestamps of the target CFR can be graphed for mounted clips and duplicated / dropped frames are indicated in the Gallery. 

- **Quad bayer demosaic**

DNGs of unbinned captures without prior remosaic are still incompatible with most raw image editors. This refers to sensors with color filter arrays (CFA) where each color filter is subdivided by 4, 9 or 16 photosites, while just one used to be standard. Active demosaic outputs raw RGB images with options to counter OCL / 4PD shading or color aliasing since unbinned output varies considerably across different sensors. Further remosaic is optional. Also binning can be performed by averaging pixel values to compare binned resolution and SNR to unbinned. There is also an option to mislable higher CFA clips as usual 2x2 bayer if they wont open with untouched CFA in a chosen editor.

- **Exposure normalization & smoothing**
   
Exposure changes between frames caused by changes in exposure settings like ISO and shutter speed are neutralised by a per frame gain, relying on the per-frame **Baseline Exposure** DNG tag, recognized by Davinci Resolve and the integrated Gallery. Additionally a static exposure offset can be typed. Exposure transitions based on the same exposure settings can also be smoothed, making jumpy auto exposure or rough manual adjustments less distracting. Smoothing is also available for white balance. 

---

### Vignette correction

To correct for vignetting in the image, some cameras store low resolution gainmaps in metadata during capture. The gainmap determines the required local compensation gain to be applied to pixel values after alignment. With separate gainmaps per color channel, color shifting in a radial gradient similar to vignetting gets compensated for as well.

- **Baking gainmaps**
  
Applies gainmap-derived local gain to pixel values. For resulting DNG files this constitutes a loss in dynamic range since multiplied pixel values may exceed the original clipping point of the image resulting in introduced clipping in frame corners. This is required to make use of vignette correction in Davinci Resolve. This behaviour is mimicked by the Gallery for now.

- **Resample gainmaps**
  
When including gainmaps in DNG opcodelist 2 / 3 metadata, many image editors tend to stretch gainmaps to fit, disregarding potential cropping of the frame and in turn causing misalignment. Due to the gainmap's low resolution croppping alone isnt viable. Therefore a resampled crop is performed resulting in gainmaps compatible to cropped footage. To retain untouched gainmap values choose Uncropped.

- **Reduce to color correction**
  
If separate gainmaps per color channel are present, their local minimal gain determines the intensity of the vignette correction. Dividing gains by their local minima isolates the color correction of the gainmap. Decreased to be applied gains retain natural vignetting and in turn more dynamic range after bake. Also when a color reduced gainmap is baked, the intensity remnant of the vignette correction is included in DNG opcodelist 3.

- **Optimize gainmaps**

To remove unwanted global offsets in gainmaps, global minimal per color channel are determined. The resulting RGB vector is isolated into color and intensity to offset white balance and BaselineExposure respectively per DNG.  

- **Debug Views**  
  - Formerly known as 'Don't clip highlights', Normalize gainmaps scales retained or baked gainmaps to a normalized state. With baking, clipped highlights appear pink for inspection.
  - Gainmaps only will apply the vignette correction to a white image. This allows to inspect the impact of the vignette correction on the underlying image data. The resulting flat field DNGs can even be used in RawTherapee for manual vignette correction.
 
---

### Calibration

JSON sidecars per mounted clips were introduced to persistently store calibration overrides and other metadata without altering original source files. Color, forward, and calibration matrices as well as asShotNeutral, cfaPhase, and orientation can be overridden along with some more options. On Create JSON an example JSON file is written in source clip directory sharing its file name. Uncomment _ prefix on example fields to enable overrides and for calibration to load.

- **Define quad bayer CFA**

To make use of aforementioned quad bayer demosaic options, clips need to be identified as such using the JSON cfaSize parameter set to 4, 6 or 8 for quad, nona or hecadecimal bayer CFA footage respectively. 2 would be the default value for normal bayer.

- **Gainmap alignment**

Theres an automatic ordering fix for a potential color channel mismatch of DNG gainmaps. needGainMapOrderFixed can be deactivated if not effective. Also fullSensorResolution is useful for cropped DNGs containing uncropped gainmaps that dont contain original frame dimensions to suitably scale the gainmap.

- **Override data levels**
  
Recognised white and black levels will default to dynamic, per-frame tags stored in the clip. Static tags are also available as a fallback for MCRAW sources. This affects the recognised input and output bitdepth of DNGs and can be manually overridden with optional hybrid options like `4095/Dynamic` when only dynamic white level is false. Apart from the usual raw data levels, `Full` or `Limited` data levels can be overridden for video sources before potential further white / black level adjustments.

- **Cropping**

Remove potential overscan and or fix overallocated raw buffers during capture with a top left crop and a optional row stride override.

- **Misc**

There is the option to ignore forward matrices, so Gallery falls back to chromatic adaptation using color matrices for color transform. Also a pattern or individual coordinates of PDAF remnant pattern or hot pixels to be stored in frame metadata or to be baked into the frame.

- **DNG sidecars**

To apply manual vignette correction by supplying a suitable white image measurement, DNG sidecars named <source_name>_white.dng are ingested per clip to be converted to gainmaps for the output DNG with usual processing all available. A sidecar named <source_name>_opcode.dng copies its opcode lists (including gain maps and lens distortion), matrices, and asShotNeutral.

- **DCP camera profiles**

Place `<source_name>.dcp` beside the clip, or set `"dcp": "profile.dcp"` in its JSON sidecar. The sibling DCP takes precedence over the JSON reference and over profile fields in a DNG sidecar; explicit JSON matrix overrides still take precedence. Gallery and thumbnails use the DCP color and forward matrices, hue/saturation maps, and look table. Tone curves and other profile fields are copied to output DNGs for compatible readers. Set `"useDcpGainmap": true` in the clip JSON to convert a spatial-only DCP gain table to OpcodeList3, replacing source and JSON gain maps in previews, preprocessing, and output DNGs. Without this switch, the DCP gain table remains profile metadata. Gain tables whose values also depend on image color or brightness remain profile metadata because a DNG GainMap opcode cannot represent that behavior.

---

### Reduction options

- **Log Transfer Curve**
  
A logarithmic transfer curve is applied to the image data with dithering. The inverse of the applied transfer curve is contained in a **Linearization Table** in DNG metadata to map the pixel values into a linear distribution with 16b precision. This efficient redistribution of pixel values allows the output bitdepth to be reduced while staying visually lossless with slightly increased noise. 10b footage can be reduced to 8b and 12/14b to 10b in a safe manner. Davinci only supports 8b and above for cfa DNGs. 

The adjacent **HQ** checkbox selects the original per-pixel triangular dither hash. With HQ off, the log curve still uses triangular dither with a cheaper spatial hash. The checkbox is available only while the log curve is enabled.

- **Proxy / Binning Mode**
  
Input resolution is being reduced by discarding pixel values to increase playback performance or by box-avergaing them to smooth the image if HQ is enabled. CFA sources are demosaiced in advance for the latter option. To reduce the resulting RGB pixel values to bayer CFA Remosaic can be enabled for better performance.

---

### Finalization

To store processed clips persistently many compression options are available. For DNG output there are lossless JPEG 92, lossy 12b JPEG DCT (Davinci only), lossless and lossy JPEG-XL options. Video compression options are available as well namely HEVC, AV1, CineForm and ProRes, relying on a RGB to YUV pixel format conversion assuming Rec.2020. JSON sidecars containing otherwise lost metadata are written automatically and resulting files can be imported again in Fuse. When specific frames from mounted clips are selected in Gallery, these can be finalized by themselves instead of the entire sequence.

Lossless JPEG DNG encoding uses Vulkan for camera-sized frames when available and falls back to the CPU otherwise. The single-scan encoder runs predictor selection, Huffman table construction, entropy packing, byte stuffing, and JPEG marker writing on the GPU; only the completed JPEG stream is read back for DNG storage. The tiled encoder accepts a resident texture or an 8–16-bit row-aligned packed GPU buffer and uses adaptive Huffman coding. CFA tiles are assembled into complete JPEGs on the GPU; RGB tiles currently use CPU assembly after the entropy payload readback. The first Vulkan encode includes device and shader setup, so the speed benefit is greatest when exporting a sequence. The "JPEG 92 Lossless" finalize format always writes tiled DNGs: it uses tiled GPU encode for supported texture or buffer output and tiled CPU encode for other processed frames. Type "JPEG92 Lossless Strip" in the format field to select the single-strip path. Set `MOTIONCAM_GPU_JPEG_DISABLE=1` to force CPU encoding. `MOTIONCAM_JPEG_TILED=1` requests the older tiled path for other eligible CFA DNGs.

When a terminal Vulkan gain or LOG bake produces 8–16-bit samples, finalization passes its packed GPU buffer directly to the tiled JPEG encoder, avoiding an uncompressed pixel readback and reupload. The encoder binds that buffer for histogram and entropy work without an extra GPU copy. The explicit strip format retains its packed-buffer GPU encoder. DirectLog can also encode a decoded GPU texture directly when no pixel bake, crop, topology conversion, or bit-depth packing is needed. With `MOTIONCAM_GALLERY_PERF_PROFILE=1`, `vulkan_gain_gpu_jpeg_handoff` and `dng_gpu_jpeg_attach` mark GPU JPEG handoffs.

For a DirectLog clip, `MotionCamFuse --directlog-jpeg-benchmark <video>` benchmarks full-resolution remosaiced CFA and RGB JPEG encoding from the decoder's Vulkan textures. It checks the CFA DNG pixels, compares the RGB JPEG byte for byte with liblj92, and compares owned versus borrowed RGB JPEG DNG construction. It also reports raw download, DNG copy, and staged JPEG times. This diagnostic uses the source textures before any configured gain or log bake.

`motioncam-finalize-interpolation-test --jpeg-gpu-benchmark` times large synthetic CFA and interleaved RGB JPEG encodes, including RGB from a GPU texture. It checks JPEG bytes against liblj92 and compares malloc-owned and borrowed-buffer RGB DNG writing byte for byte.

An experimental Vulkan lossless JPEG **decoder** for independent CFA or RGB tiles is exercised by `MOTIONCAM_REQUIRE_GPU_JPEG_DECODE_TEST=1 MOTIONCAM_GPU_JPEG_RGB_DECODE_TEST=1 ./build-linux-release/motioncam-lossless-jpeg-test`. Add `MOTIONCAM_JPEG_DECODE_RESIDENT_TEST=1` to validate decode → gain → JPEG encode with decoded pixels staying on the GPU; `MOTIONCAM_JPEG_DECODE_LOG_TEST=1` includes LOG processing. `MOTIONCAM_JPEG_DECODE_LARGE_BENCHMARK=1` runs a synthetic 12 MP image with 128×128 tiles, while `MOTIONCAM_JPEG_DECODE_SMALL_TILES=1` uses 32×32 tiles. For the HLG DirectLog clip, `MOTIONCAM_DIRECTLOG_JPEG_DECODE_BENCHMARK=1 ./build-linux-release/MotionCamFuse --directlog-jpeg-benchmark <video>` encodes actual remosaiced CFA and RGB frame pixels into independent tiles, then checks GPU decode and resident JPEG re-encode against the source. The default tile size is 64×64 for CFA and 32×32 for RGB; set `MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_SIZE` to test another size. It compares the GPU path with CPU tile decode followed by GPU JPEG encode. Contiguous input uploads JPEG bytes plus seven layout words per tile; GPU header parsing builds the full metadata. DNG import uses GPU decode for regular, full-size JPEG92 CFA or RGB tile grids, with CPU fallback for other layouts or GPU failures.

`MOTIONCAM_GPU_JPEG_REAL_DNG_BENCHMARK=<path> ./build-linux-release/motioncam-lossless-jpeg-test` inspects and CPU-decodes a real single-strip JPEG DNG. The HLG DirectLog DNG in the saved session has one 4096×3072 JPEG scan; a per-tile GPU decoder would launch only one serial entropy task for it. The experimental decoder rejects streams above 65,536 samples per tile so this layout uses the CPU path rather than stalling the GPU. Accelerating these existing DNGs requires a parallel entropy decoder, or independently tiled JPEG at DNG creation time.

`MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_BENCHMARK=both ./build-linux-release/MotionCamFuse --directlog-jpeg-benchmark <video>` feeds the GPU encoder's full CFA and RGB JPEG payloads to the decoder and compares a 128×128 crop in each mode with CPU decode. Set `MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_SIDE=256` to test a larger crop, and `MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_SKIP_FULL=1` to omit full payloads. `MOTIONCAM_GPU_JPEG_ALLOW_LARGE_SCAN=1` overrides the large-scan guard for bounded diagnostic runs only. The current full-frame encoder writes no JPEG restart markers, so one scan has no independent entropy starting points.

For tiled decode measurements, `MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_WIDTH` and `MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_HEIGHT` can override the default tile dimensions. Set `MOTIONCAM_DIRECTLOG_JPEG_GPU_TILED_BENCHMARK=1` to encode HLG CFA and RGB textures into independent GPU JPEG tiles. The `DirectLog GPU resident JPEG roundtrip` line reports the median of five warm GPU encode → decode runs; the JPEG payload stays on the GPU, and exact pixels are checked in a separate pass. The `DirectLog JPEG transfer benchmark` line includes a final JPEG readback and decoder reupload for comparison. The benchmark also checks tiled DNG roundtrips. On the measured HLG frame, 16×64 CFA and 8×128 RGB were the fastest tested tile dimensions and are the current texture-encode defaults.

- **Duplicated frame interpolation**

Duplicated frames introduced by CFR conversion can be replaced by motion-interpolated frames using [RIFE Fix Drop Frames and Convert FPS](https://github.com/may-son/RIFE-FixDropFrames-and-ConvertFPS). On first use, Fuse downloads a pinned RIFE release and installs its Python dependencies into a private application-data environment. Python 3 must be available for this one-time setup. Synthetic and duplicated frames are indicated as such in frame metadata. Beyond that duplicated frames are still detectable in DNG sequences to properly interpolate DNG sequences with poor CFR conversion.

- **MCRAW archival**

Original MCRAW source files can be compressed without processing using 7z LZMA2 Ultra and opened again in Fuse by decompressing to a temporary MCRAW on import. DNG sequences and DirectLog sources are not supported by this archival format.

---

### Preferences

There are some more options configurable under Settings > Preferences. Here virtual DNG folder mounting using FUSE/ProjectedFS dependency can be disabled. The output directory for mounted clips can be overridden to no longer use individual source clip directory. An indexing cache was introduced to accelerate access time for source clips. On Windows there is another cache limit option, since it is not capped by default like fuse3 on Linux. There are two more options to auto apply settings changes instead of applying to selected / all manually and to unmount clips on finalize.

---

### Building

Windows (Visual Studio + Qt + vcpkg):
```
git submodule update --init --recursive
.\build.bat
```
Executable lands in `build\Release\MotionCamFuse.exe`.

macOS (Qt + vcpkg, CMake):
```
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

macOS x86_64 (Intel or Hackintosh):
```
git submodule update --init --recursive
cmake -S . -B build-x86 -DCMAKE_BUILD_TYPE=Release -DMOTIONCAMFUSE_MACOS_X86_64=ON -DVCPKG_TARGET_TRIPLET=x64-osx
cmake --build build-x86 --config Release
```

Universal macOS (x86_64 + arm64) can be built by adding `-DMOTIONCAMFUSE_MACOS_UNIVERSAL=ON` when your dependencies are available as universal binaries.

Linux x86_64:

```bash
sudo apt install clang ninja-build pkg-config libfuse3-dev libboost-filesystem-dev libboost-locale-dev libboost-regex-dev libspdlog-dev libfmt-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel
```

---

### Docs

See `HELP.md` for usage, preferences, and feature details.

Gallery playback profiling can be run from a release build with:

```bash
MOTIONCAM_GALLERY_PERF_PROFILE=1 MOTIONCAM_GALLERY_PERF_SKIP_BACKFILL=1 \
  MOTIONCAM_GALLERY_PERF_PRESERVE_PROXY=1 \
  ./build/MotionCamFuse --gallery-perf-session /path/to/session.json \
  --gallery-perf-playback-ms 3000
```

The runner enables the histogram, keeps its overlay visible throughout the
run, and logs per-frame materialization, color processing, and histogram
timings. `MOTIONCAM_GALLERY_PERF_PRESERVE_PROXY=1`
uses the session's draft scale with HQ off; omit it for full-resolution playback.
`MOTIONCAM_GALLERY_PERF_SKIP_BACKFILL=1` keeps the run focused on playback and
seeks; omit it to include paused thumbnail backfill.
The performance runner polls presentation every 1 ms instead of pacing it to
the clip frame rate, so its playback samples measure gallery throughput.
Set `MOTIONCAM_GALLERY_PERF_MODE=mounted` to run the mounted DNG sequence
benchmark instead of gallery playback. It reads frames 0, 1, and 2, seeks to
the midpoint, then reads the next frame for each clip. Its
`mounted_sequence_sample` log records full-file FUSE read time and DNG preview
decode time separately. Mounted reads run on a worker thread so the UI event
loop remains available to service mounts. Each invocation runs only one mode;
mounted mode also disables thumbnail generation.
Set `MOTIONCAM_GALLERY_PERF_MOUNTED_SEQUENTIAL_COUNT=5` to sample a fixed
number of consecutive frames per clip. Set
`MOTIONCAM_GALLERY_PERF_COMPRESS_JXL=1` to pass each mounted DNG through the
JPEG XL compression stage of `finalizeDng` using the clip's configured distance
before preview decoding. `jxl_encode_sample` records compression time and DNG
sizes; `mounted_sequence_sample` then records decode time for that output.
This measures per-frame finalization compression without writing a finalized
clip to disk.
Set `MOTIONCAM_GALLERY_PERF_PACK_TO_WHITE_LEVEL=1` to include sensor bit-depth
packing in that comparison. The JXL path normally fuses clipping with input
preparation. With profiling enabled,
`jxl_compress_stage` separates TIFF parsing and sample preparation, libjxl
encoding, and DNG strip replacement. Packed 10-, 12-, and 14-bit CFA and
LinearRaw strips use grouped sample decoding before libjxl encoding.
`mounted_decode_breakdown` separates DNG layout, optional dump, preview decode, and the
optional validation hash. Set `MOTIONCAM_GALLERY_PERF_HASH_PREVIEW=1` when
comparing decoded pixels; the hash is omitted by default because it is much
slower than the decode on full-size frames.
`MOTIONCAM_JXL_PACK_BENCH=1 ./build-linux-release/motioncam-jpeg-xl-test`
compares both paths on 12 MP synthetic 10-, 12-, and 14-bit DNGs.
Set `MOTIONCAM_GALLERY_PERF_JXL_WORKERS=N` (0–16) to compare libjxl worker
counts during profiling. The normal cap is eight workers for both decode and
encode, limited by the available CPU threads.
Set `MOTIONCAM_GALLERY_PERF_PARALLEL_READS=1` in mounted mode to add four
simultaneous cold reads per clip; the `mounted_parallel_sample` events expose
queueing that sequential reads cannot measure.
Set `MOTIONCAM_GALLERY_PERF_PARALLEL_READ_COUNT=12` to test a larger read-ahead
burst; the count is capped at 16 and applies only in mounted mode.
With `MOTIONCAM_GALLERY_PERF_PROFILE=1`, `mounted_frame_stage` reports the same
queue, load, DNG generation, shared pixel processing, finalization, and total
times for MCRAW, DirectLog, and DNG sources. `source_path` separates clips of
the same source type.
Set `MOTIONCAM_GALLERY_PERF_DUMP_DNG=/tmp/mounted-first.dng` to retain the first
mounted frame for inspection of its TIFF tags.
`MOTIONCAM_GALLERY_PERF_NO_HISTOGRAM=1` measures the same run without retaining
raw samples or drawing the histogram.
`MOTIONCAM_GALLERY_PERF_HIDE_THUMBNAILS=1` hides the thumbnail row and disables
its frame collection during playback.
`MOTIONCAM_GALLERY_PERF_VIEWPORT=960x540` tests a fixed window size. Sequence
frames, including full-resolution frames, are presented at source size through
Qt Quick's Vulkan scene graph when available; Qt uses its native graphics
backend on macOS. Set `MOTIONCAM_GALLERY_FFMPEG_PRESENT=1` to compare the
previous FFmpeg presenter.

---

### MotionCam DirectLog Camera Native Guide

Camera Native captures omit color transforms and white balance during image processing before video encode. Resulting frames retain white balance invariance with limited highlight reconstruction. When a mounted clip gets finalized as video, otherwise omitted metadata is written to a JSON sidecar for the resulting file. However native video captured in MotionCam does not retain auto white balance, dynamic vignette correction, and exposure normalisation since metadata is not captured for now. Static metadata can be supplied with DNG / JSON sidecar files preferably captured in same environment as Native clips.

In MotionCam use Camera Native color space option, HLG transfer curve, 2,0 EV gain, all tonemapping disabled, variable frame rate, full data levels. Linear transfer curve is default for Camera Native but its inefficient nature for integer value distribution make it useless for capture. Vignette correction and highlight reconstruction is disabled for Native, but these restrictions are ignored on MCRAW video export.

JSON sidecar also allow to set a DNG path. Further JSON parameters, beyond what a DNG sidecar can provide, are useful for DirectLog captures like centeredCrop for encoder overscan, dataLevels (currently 8bit full level encodes are mislabled as such, while levels are limited), and levels that apply to normalized value range of 65535/0 for DirectLog sources after dataLevels (useful if capture used an incorrect gain resulting in lowered clipping point).



- **Bad and PDAF Pixels (unfinished)**

Calibration sidecars may contain a `badPixels` array. `start` names one full-sensor position or the absolute origin of a repeating PDAF lattice. `repeat` sets its horizontal and vertical spacing; optional `end` gives its inclusive full-sensor `[x,y]` limit. `treatment` is `brighten`, `dampen`, or `interpolate`; adjustment `amount` and `threshold.above`/`threshold.below` accept normalized values or percentages. Thresholds are evaluated against that pixel's own black-subtracted value, normalized so black is 0 and white is 1. Optional `minIso` and `minExposure` gates accept values such as `800`, `"1/30"`, or `"20ms"`. The Bad Pixel Treatment selector can bake corrections, emit eligible interpolation positions in DNG OpcodeList1 while retaining stored raw samples, transfer the source `badPixels` array and `fullSensorResolution` to a finalized CFA DNG clip's JSON sidecar (or to one JSON sidecar per selected CFA frame) while retaining stored raw samples, mark treated samples full black for inspection, or disable treatment. Demosaiced output bakes the corrections. Vignette Correction set to Resample also bakes enabled bad-pixel treatment, regardless of the Bad Pixel Treatment selector; Disabled remains disabled. Repeating entries use `{ "start": [13, 48], "repeat": [16, 16], "end": [4095, 3071], "treatment": "interpolate" }`.

# MotionCam Fuse – Virtual File System

> **Work in Progress**

**MotionCam Fuse** allows opening raw image and video files, such as MCRAW. These are mounted as projected folders containing DNG sequences. Several processing and calibration options aim to provide an optimal raw editing workflow—preferably in **Davinci Resolve** for video. Clips can also be inspected in an integrated Gallery, allowing for playback and specific frame selections. To save processed files to disk as intermediates or for archival, mounted clips may be finalized with many video and DNG compression options. 

---

<img width="1531" height="724" alt="image" src="https://github.com/user-attachments/assets/880479b3-142f-4a54-9ce1-bf63b59b6c5c" />

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

To apply manual vignette correction by supplying a suitable white image measurement, DNG sidecars named <source_name>_white.dng are ingested per clip to be converted to gainmaps for the output DNG with usual processing all available. A sidecar named <source_name>_gainmap.dng instead copies included gainmaps as well as matrices and asShotNeutral.

---

### Reduction options

- **Log Transfer Curve**
  
A logarithmic transfer curve is applied to the image data with dithering. The inverse of the applied transfer curve is contained in a **Linearization Table** in DNG metadata to map the pixel values into a linear distribution with 16b precision. This efficient redistribution of pixel values allows the output bitdepth to be reduced while staying visually lossless with slightly increased noise. 10b footage can be reduced to 8b and 12/14b to 10b in a safe manner. Davinci only supports 8b and above for cfa DNGs. 

- **Proxy / Binning Mode**
  
Input resolution is being reduced by discarding pixel values to increase playback performance or by box-avergaing them to smooth the image if HQ is enabled. CFA sources are demosaiced in advance for the latter option. To reduce the resulting RGB pixel values to bayer CFA Remosaic can be enabled for better performance.

---

### Finalization

To store processed clips persistently many compression options are available. For DNG output there are lossless JPEG 92, lossy 12b JPEG DCT (Davinci only), lossless and lossy JPEG-XL options. Video compression options are available as well namely HEVC, AV1, CineForm and ProRes, relying on a RGB to YUV pixel format conversion assuming Rec.2020. JSON sidecars containing otherwise lost metadata are written automatically and resulting files can be imported again in Fuse. When specific frames from mounted clips are selected in Gallery, these can be finalized by themselves instead of the entire sequence.

- **Duplicated frame interpolation**

Duplicated frames introduced by CFR conversion can be replaced by motion-interpolated frames using [RIFE Fix Drop Frames and Convert FPS](https://github.com/may-son/RIFE-FixDropFrames-and-ConvertFPS). On first use, Fuse downloads a pinned RIFE release and installs its Python dependencies into a private application-data environment. Python 3 must be available for this one-time setup. Synthetic and duplicated frames are indicated as such in frame metadata. Beyond that duplicated frames are still detectable in DNG sequences to properly interpolate DNG sequences with poor CFR conversion.

- **MCRAW archival**

MCRAW clips can be compressed using 7z LZMA2 Ultra and opened again in Fuse by decompressing to a temporary MCRAW on import.

---

### Preferences

There are some more options configurable under Settings > Preferences. The output directory for mounted clips can be overridden to no longer use individual source clip directory. An indexing cache was introduced to accelerate access time for source clips. On Windows there is another cache limit option, since it is not capped by default like fuse3 on Linux. There are two more options to auto apply settings changes instead of applying to selected / all manually and to unmount clips on finalize.

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

---

### MotionCam DirectLog Camera Native Guide

Camera Native captures omit color transforms and white balance during image processing before video encode. With required metadata supplied frames retain white balance invariance with possible highlight reconstruction. MotionCam Fuse writes necessary metadata for its processing options to a JSON Sidecar when finalizing with Native options. However for now on MotionCam Native captures auto white balance, dynamic vignette correction, and exposure normalisation are not possible since metadata is not captured. 

Use Camera Native color space option and HLG transfer curve at 1,5 EV gain, all tonemapping disabled, variable frame rate, full data levels. Linear transfer curve is default for Camera Native but its inefficient value distribution make it useless for capture. Vignette correction and highlight reconstruction is disabled for Native but these restrictions are ignored on MCRAW video export.

Before or after Native capture, capture DNG / MCRAW to use contained matrices, white balance and vignette correctoin like mentioned at DNG sidecars above. Further JSON parameters are useful for DirectLog captures like centeredCrop for encoder overscan, dataLevels (currently 8bit full level encodes are mislabled as such, while levels are limited), and levels that apply to normalized value range of 65535/0 for DirectLog sources after dataLevels (useful if capture used an incorrect gain resulting in lowered clipping point).



- **Bad and PDAF Pixels (unfinished)**

Calibration sidecars may contain a `badPixels` array. `start` names one full-sensor position or the absolute origin of a repeating PDAF lattice. `repeat` sets its horizontal and vertical spacing; optional `end` gives its inclusive full-sensor `[x,y]` limit. `treatment` is `brighten`, `dampen`, or `interpolate`; adjustment `amount` and `threshold.above`/`threshold.below` accept normalized values or percentages. Thresholds are evaluated against that pixel's own black-subtracted value, normalized so black is 0 and white is 1. Optional `minIso` and `minExposure` gates accept values such as `800`, `"1/30"`, or `"20ms"`. The Bad Pixel Treatment selector can bake corrections, emit eligible interpolation positions in DNG OpcodeList1 while retaining stored raw samples, mark treated samples full black for inspection, or disable treatment. Repeating entries use `{ "start": [13, 48], "repeat": [16, 16], "end": [4095, 3071], "treatment": "interpolate" }`.

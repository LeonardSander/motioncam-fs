# MotionCam Fuse – Virtual File System

> **Work in Progress**

**MotionCam Fuse** allows opening raw image and video files, such as MCRAW. These are mounted as projected folders containing DNG sequences. Several processing and calibration options aim to provide an optimal raw editing workflow—preferably in **Davinci Resolve** for video. Clips can also be inspected in an integrated Gallery, allowing for playback and specific frame selections. To save processed files to disk as intermediates or for archival, mounted clips may be finalized with many video and DNG compression options. 

---

<img width="1531" height="724" alt="image" src="https://github.com/user-attachments/assets/880479b3-142f-4a54-9ce1-bf63b59b6c5c" />

---

When clips are mounted, folders containing respective frames will appear in their source directory if not defined otherwise under Settings > Preferences. As soon as the individual frames are accessed, their image contents are processed and cached.

For Windows builds the output directory needs to be on a NTFS drive. Also on Windows cached files are written into the projected folder structure and may remain along empty projected files when Fuse is closed irregularly or if Discard on Unmount is disabled under Preferences. These files will be properly accessible again when Fuse is opened again and the session is resumed. The cache can be manually cleared by closing Fuse, deleting folder contents first and folders afterwards.

---

### Features

[Showcase video](https://youtu.be/knACG5jy-rk)

- **Constant frame rate conversion**

To account for non-standard or variable frame rates (VFR), a conversion to a suitable standard constant frame rate (CFR) is implemented. Based on the target frame rate, derived from a clip's median frame rate, captured frames are dynamically duplicated or dropped. This conversion ensures real time playback avoiding desync in a CFR editing timeline like Davinci Resolve. For non-real time playback a clip's median frame rate can be chosen as target as well. The per frame timing deviation from timestamps of the target CFR can be graphed for mounted clips and duplicated / dropped frames are indicated in the Gallery. 

- **Quad bayer demosaic**

DNGs of unbinned captures without prior remosaic are still incompatible with most raw image editors. This refers to sensors with color filter arrays (CFA) where each color filter is subdivided by 4, 9 or 16 photosites, while just one used to be standard. Active demosaic outputs raw RGB images with options to counter OCL shading or color aliasing since unbinned output varies considerably across different sensors. Further remosaic is optional. Also binning can be performed by averaging pixel values to compare binned resolution and SNR to unbinned. There is also an option to mislable higher CFA clips as usual 2x2 bayer if they wont open with untouched CFA in a chosen editor.

- **Exposure normalization & smoothing**
   
Exposure changes between frames caused by changes in exposure settings like ISO and shutter speed are neutralised by a per frame gain, relying on the per-frame **Baseline Exposure** DNG tag, recognized by Davinci Resolve and the integrated Gallery. Additionally a static exposure offset can be typed. Exposure transitions based on the same exposure settings can also be smoothed, making jumpy auto exposure or rough manual adjustments less distracting. Smoothing is also available for white balance. 

---

### Vignette Correction

- **Baking Vignette Correction**
  
Apply gainmap vignette correction metadata contained in MCRAW per frame to pixel values. Alongside compensating for vignetting, color correction will be performed in image corners by applying different gainmaps per color channel. This phenomenon is visible as color casting in a radial gradient similar to the vignetting and varies per lens. Saving the gainmaps as Opcode metadata in DNGs instead of applying them to pixel values is not usable yet.

- **Reduce to Color Correction**
  
  Here the gainmap metadata is modified before being applied to the image to retain natural vignetting and dynamic range in image corners.

- **Debug Views**  
  - Formerly known as 'Don't clip highlights' the Scale data option allows to inspect clipping in the image by applying the vignette correction in a normalized state with clipped highlights showing pink.
  - Gainmaps only will apply the vignette correction to a white image. This allows to inspect the impact of the vignette correction on the underlying image data. The resulting flat field DNGs can even be used in RawTherapee for manual vignette correction.
 
---

### Further Preprocessing

- **Log Transfer Curve**
  
  A logarithmic transfer curve is applied to the image data with dithering. The inverse of the transfer curve is contained in a **Linearization Table** in DNG metadata to map the pixel values into a linear distribution with 16b precision. This efficient redistribution of pixel values allows the output bitdepth to be reduced while staying visually lossless with slightly increased noise. 10b footage can be reduced to 8b and 12/14b to 10b in a safe manner. Davinci only supports 8b and above. The transfer curve is applied alongside the vignette correction so the quantizational rounding error is only realized once. So there is no reason not to use it when baking vignette correction.

- **Proxy / Binning Mode**
  
  This mode reduces image resolution for editing. HQ demosaics ordinary Bayer footage first and box-averages the resulting RGB pixels. Quad Bayer has a dedicated path: 2x averages each same-color 2x2 block directly into ordinary Bayer, while 4x/8x demosaic that binned Bayer and apply the remaining RGB reduction. DirectLog and RGB DNG inputs use RGB averaging. With HQ disabled, RGB inputs retain a representative pixel per reduction block and Bayer proxy processing uses its fast sample-selection path. Remosaic may be enabled after RGB reduction to convert the result back to ordinary Bayer.

- **Off Center Cropping**
  
  16:9 sensor modes are commonly used by modern devices when 60fps capture is requested. However many of these devices do not provide the suitable raw output configuration which results in an captured image with a buffer underflow. The empty data recorded in the bottom part of the image can be conveniently cropped out using this option. This is the only way to have properly aligned vignette correction on a capture like this. Only full sensor captures without cropping are compatible (Also reselect lens when choosing 60fps slot to not capture junk data in underflown image area).

- **Higher CFA Support**
  
  Fuse detects the CFA repeat size from MCRAW metadata (`cfaSize`, with the legacy remosaic flag mapping to 4x4), DNG CFA tags, or the per-clip JSON sidecar. The Higher CFA Processing control can demosaic higher-CFA footage to RGB, use the **Demosaic (Color)** variant to reconstruct green/luminance before interpolating colour differences and remove coherent 2x2 mean/phase gain errors while retaining irregular fine detail, use an OCL/4PD-oriented anti-aliasing variant, retain correct 4x4/6x6/8x8 CFA metadata, or deliberately label it as ordinary 2x2 Bayer for compatibility. Enabling Remosaic converts a demosaiced result back to ordinary Bayer for applications that do not accept RGB DNGs.

  With **HQ** enabled, ordinary Bayer and RGB inputs are reduced by averaging RGB blocks without changing black or white levels. Quad Bayer is first averaged into half-resolution ordinary Bayer; at 2x that Bayer image is the output, while 4x/8x demosaic it before the remaining reduction. With HQ disabled, Fuse uses the faster sample-selection reduction.

- **Bad and PDAF Pixels**

  Calibration sidecars may contain a `badPixels` array. An entry names one full-sensor position, or a position inside a repeating PDAF tile with `repeat`. `treatment` is `brighten`, `dampen`, or `interpolate`; adjustment `amount` and `threshold.above`/`threshold.below` accept normalized values or percentages. Thresholds are evaluated against that pixel's own black-subtracted value, normalized so black is 0 and white is 1. Optional `minIso` and `minExposure` gates accept values such as `800`, `"1/30"`, or `"20ms"`. The Bad Pixel Treatment selector can bake corrections, emit eligible interpolation positions in DNG OpcodeList1 while retaining stored raw samples, or disable treatment. Repeating entries use `{ "x": 3, "y": 5, "repeat": [16, 16], "treatment": "brighten", "amount": "12%", "threshold": { "below": "75%" } }`.

  In LQ mode, higher-CFA proxy sampling remains aligned to complete same-color blocks: 6x6 CFA uses its 3x3 color blocks, while 8x8 CFA can use staged 2x reduction for demosaic modes or complete 4x4 blocks at 4x proxy. HQ treatment of non-quad higher-CFA footage demosaics the full image before applying the requested RGB reduction factor.

---

### Linux build and AppImage

Linux requires FUSE3 (the user must be permitted to mount FUSE filesystems), Qt 6, Boost filesystem/locale/regex, fmt, spdlog, and Clang. On Debian/Ubuntu, install the native development packages and configure:

```bash
sudo apt install clang ninja-build pkg-config libfuse3-dev libboost-filesystem-dev libboost-locale-dev libboost-regex-dev libspdlog-dev libfmt-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel
```


[Changelog](https://discord.com/channels/980884979955421255/1377309561219973121/1420903594198040717) in reply chain

If these links do not open you need to join the [MotionCam Discord Community](https://discord.gg/Vy4gQNEdNS) first.

⚠️ **Note:** Expect slowdowns when opening MCRAW files or changing settings.

# Master branch README (merged suffix)

# MotionCam Fuse (Fuse-2026)

Desktop tool for mounting MotionCam recordings, exporting DNGs, and reviewing clips.

## Build

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

## Docs

See `HELP.md` for usage, preferences, and feature details.

## Linux

Linux requires FUSE3 (the user must be permitted to mount FUSE filesystems), Qt 6, Boost filesystem/locale/regex, fmt, spdlog, and Clang. On Debian/Ubuntu:

```bash
sudo apt install clang ninja-build pkg-config libfuse3-dev libboost-filesystem-dev libboost-locale-dev libboost-regex-dev libspdlog-dev libfmt-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel
```




CFR holds can optionally be replaced during DNG or Camera Native finalization with motion-interpolated frames from [RIFE Fix Drop Frames and Convert FPS](https://github.com/may-son/RIFE-FixDropFrames-and-ConvertFPS). On first use, Fuse downloads a pinned RIFE release and installs its Python dependencies into a private application-data environment; Python 3 must be available for this one-time setup. The model is loaded once per finalization and processes one dropped-frame run at a time so only its two endpoint tensors are resident. CFA DNGs are converted to 16-bit RGB for inference; CFA output modes are remosaiced afterward. Exposure time, ISO, baseline exposure, and white balance are interpolated with the pixels. CFR hold DNGs carry `rpt:DuplicateFrame='true'`; interpolated DNGs carry `rpt:SyntheticFrame='true'` and set the duplicate flag to false. Camera Native JSON sidecars preserve both states per frame as `duplicateFrame` and `syntheticFrame`.

Camera Native finalization can encode LOG60 as HEVC/MOV, AV1/MP4, ProRes LT/Standard/HQ in 10-bit YUV 4:2:2, or CineForm in 10-bit YUV 4:2:2 or 12-bit RGB. The RGB CineForm path does not perform an RGB-to-YUV conversion. These modes also accept DirectLog inputs. MP4 is used for AV1 because FFmpeg does not support AV1 in QuickTime MOV; its 1 MHz video track time scale retains sub-millisecond VFR presentation timestamps. The standard AV1 profile uses `libsvtav1` at CRF 7 and preset 3. The AV1 HDR + Noise profile uses CRF 12, preset 2, and the ABI-compatible SVT-AV1-HDR fork's film-grain tune and generated noise table; it requires an FFmpeg executable built against SVT-AV1-HDR.


- **Override Data Levels**
  
  White and Black Levels used in Fuse will default to their dynamic tags stored in MCRAW. Static tags are also available as a fallback (choose that to apply the levels override from calibration.json). White and black sources can be mixed independently, for example `Static/Dynamic`, `Dynamic/Static`, `1023/Dynamic`, or `Static/64`. DNG levels can also be overridden; for DNG inputs, both Dynamic and Static refer to the levels stored in each individual DNG. DirectLog uses its separate input-level handling.


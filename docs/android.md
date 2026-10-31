# Android arm64 notes

## Distribution and use

The `android-apk` workflow builds a single `arm64-v8a` APK with Qt 6.12.0 and
the same pinned vcpkg dependency snapshot as the desktop jobs. The package is
signed with a debug key, so it can be sideloaded from a GitHub Actions artifact.
It is not a stable upgrade identity across different builders; a public release
needs a persistent release keystore and `QT_ANDROID_SIGN_APK`.

Android cannot expose app-owned FUSE mounts. The `ProcessingFileSystem`
backend keeps imported clips in-process for metadata, gallery previews, and
finalization. The Android **Import clip** action uses the system picker. For a
seekable document provider, Fuse retains its descriptor and exposes a
process-local `/proc/<pid>/fd` link with the original extension, so path-based
decoders can seek the source without copying the full clip. Providers that
cannot supply a seekable descriptor use a private-file copy fallback. This
path handles single-file clips;
DNG sequences use the document-tree folder import flow. File and folder imports
resolve source grants through the same import-directory identity, including
source-directory finalization and sidecar refresh.
**Scan directory for sidecars** grants access to a chosen source
directory and scans that directory for already imported clip filenames. It
copies adjacent files with the same clip stem into that clip's private import
directory, preserving their original names and extensions. This includes
calibration JSON, Gyroflow JSON, GCSV, PP3, XML, and other matching sidecars;
clip media files are excluded. The scan also fetches exact relative files
named by the per-clip JSON's `dng_white`, `dng_opcode`, `dcp`, and `gyroflow`
fields. It does not otherwise descend into subdirectories. Run the scan again
when importing clips from a
different source directory. Imported clip metadata and previews refresh after
the scan. The scan requires a directory selection, not a selection for each
sidecar. The clip card displays the source directory; private storage remains
the decoder's working copy. On mount or refresh, private sidecars are retired
and resynced from the granted source directory; older private-only sidecars
are preserved in `legacy-sidecars` and cannot drive calibration status.
**Create JSON** writes the per-clip file to the granted source directory and
refreshes the working copy. If that clip has no linked source directory,
**Create JSON** opens the directory picker and continues after the clip is
found. Previously granted directories are reused for other clips in the same
folder. For newly imported clips, the picker starts at the clip's recorded
source location when the provider exposes it. Double tapping **Calibration
Loaded** or **Calibration Ignored** opens the source JSON in an external
Android text editor; returning to Fuse refreshes calibration. A single-file
picker grant cannot create an adjacent document.

Finalization streams each named output entry through the shared native
finalizer to Android's document provider. The first export asks for a retained
document-tree grant; later exports reuse it. Destination preferences can select
Documents, the source directory, or a custom granted directory. The output
bytes are passed synchronously through a direct byte buffer, avoiding an
app-private copy of the whole finalized sequence. Camera Native and 7z modes
depend on external executables and are not part of this Android export path.

The main window and gallery follow all four sensor rotations. In portrait, the
main window stacks the clip list above settings, initially giving each half of
the screen; the draggable divider keeps one sixth as the minimum pane size. In
landscape, the panes sit side by side with the same proportions. The clip list
scrolls by touch, and the settings pane scrolls horizontally with an
independent vertical scroll for each column. Scrollbars and kinetic overshoot
are disabled, and settings checkboxes toggle only from their indicator.
Preferences and frame timing stay in landscape, including the reverse
landscape rotation. The gallery covers the full-screen main window in the same
Android surface, hides secondary windows, and restores them on close. Controls
receive direct touch input.

On Android, the button beside **Import clip** opens the session actions: New,
Load, Save, Save As, and Recent Sessions. **Preferences** appears below the
sidecar scan button. Saved sessions live in the app's private session directory;
Load lists them in the app and Save As asks for a session name.

The local arm64 APK build was verified with Qt 6.12.0, a full JDK 21,
Android SDK platform and build tools 36, and NDK r27c. The SDK must contain
`platforms/android-36` and `build-tools/36.0.0`. The APK is signed with a
debug key. The Android vcpkg overlay builds libplacebo 7.360.1 and FFmpeg
with avfilter support so DirectLog's Vulkan conversion and gallery color
processing can run on compatible devices. Android uses the native Vulkan
gallery presenter inside the existing Qt activity surface. The same presenter is
selected by default on supported desktop builds. Gallery frames are presented
by its Vulkan QWindow and swapchain; QWidget only hosts the window and controls.
Android does not use a QWidget image-painting fallback. Qt backing-store backend
settings are scoped to Android and do not override desktop backend selection.
On Android, decoding, gallery processing and native presentation share one
Vulkan device, with separate libplacebo command recorders and common queue
locks. The presenter wraps the processed VkImage directly, with an in-device
timeline semaphore for readiness. There are no exported memory descriptors or
inter-device semaphore transfers. Unsupported image output retains the host
upload fallback.
Desktop retains its existing device and DMA-BUF transport.

## Current processing and lifetime rules

Gallery compute shaders are created on demand. RGB texture shaders exclude
Bayer demosaic branches, and decoder devices share libplacebo's compiled shader
cache in memory and in the application's cache directory across launches.
Small previews already in host memory use the existing CPU color transform.

Gallery host-input and thumbnail-readback policy is shared across platforms.
Vulkan memory topology selects direct host-visible compute input and deferred
thumbnail readback on unified-memory GPUs; discrete GPUs retain staging and
synchronous thumbnail readback. Allocation failures retain the staging fallback.
Queued thumbnails carry the playback generation and are discarded after a seek
or clip replacement.
MediaCodec hardware-buffer import and native surface creation remain
platform adapters, independent of that memory policy. macOS requires validation
on hardware; Android testing does not validate its import/presentation backend.

DirectLog uses MediaCodec AImageReader output when FFmpeg Vulkan decode is
unavailable. Hardware-buffer imports and submission resources are cached, with
at most three output images. The intermediate is RGBA16 UNORM; displayed 10-bit
frames use packed RGB10A2 UNORM. Full-resolution deferred RGB decoding fuses
HLG/LOG60 linearization into hardware-buffer sampling, so the gallery consumes
that texture without a second full-frame libplacebo transfer pass. Scaled and
CFA output retain the existing conversion path. No OpenGL presenter is used.
Resident RGB clipping is classified in the gallery color shader, including
thumbnail output, without a full-frame source readback. CPU fallback still
builds its clipping mask from source samples.

Controls and thumbnails are captured at the native display pixel ratio into a
compact overlay atlas; empty screen areas are not uploaded. The same capture
and refresh policy is used on desktop and Android. Unchanged controls reuse
cached native-resolution pixels. The thumbnail strip uses the same Qt item view
and delegate on both platforms, drawing visible items without per-frame widget
creation or recycling. Complete native-resolution cells are cached within a
16 MiB budget; the current-frame highlight is drawn separately. Android sizes aspect-correct thumbnail cells to fit at least three in portrait
and five in landscape. Only the thumbnail strip spans the full window width;
the title, histogram, buttons and seek bar retain their small fixed margins
without reserving front-camera cutout insets.
Android main-window outer padding and scroll-area borders/padding are removed.
A landscape histogram further limits thumbnail height. Desktop sizing is
unchanged.
Capture is bounded by the viewport, regardless of clip length. Thumbnail image caching is
bounded by both 4096 frames and 128 MiB.

Widgets remain active as input targets while Vulkan owns presentation. Ordinary
backing-store updates are suspended for the gallery window and restored on
close or presenter fallback. Offscreen capture temporarily permits paint events
without scheduling an on-screen repaint. Qt's ordinary opacity effect is
disabled during native capture; fades blend compact captured bands instead.
During playback, the controls are captured before the video handoff and join the
same presentation; paused or stalled playback permits independent refreshes.
Capped muted playback buffers up to three frames before starting its cadence,
while the first preview stays visible.
Paused seeks, frame stepping and end of stream bypass prebuffering. Uncapped
playback retains its shallow queue and does not enforce a frame-rate limit.
The FPS label refreshes at most four times per second without changing the
underlying presentation-interval samples.

Regular tiled lossless-JPEG DNGs reuse the existing Vulkan decoder and direct
CFA output shader. Compatible full-scale previews keep samples resident through
the shared gallery color pass, including source linearization, gain maps and
LOG processing. Crop, proxy scaling, higher-CFA processing and incompatible
preprocessing retain the shared CPU fallback. CFA and RGB sources use one
DNGGpuImage storage/lifetime record, including recovery readback.

Shared textures retain their processing resources and source decoder/device
lease until presentation has finished using them. A drain waits for the render
thread to release every occupied slot before returning. Switching to a still
also retires unused sequence slots.
Qt SurfaceAboutToBeDestroyed stops the renderer and releases prepared native
swapchains before the underlying Android window disappears. Desktop CPU sources
retain device-local staging on discrete GPUs; unified-memory GPUs can use
direct host-memory shader input on every platform.

Plain RGB sources bind the shared neutral gain buffer even without a bake.
Vignette selection only changes reported levels when the clip has gain maps.
Session restore refreshes clip metadata after applying local overrides.

## Finalization

Document writes use a temporary `.partial` name and rename after closing.
Checked JNI calls propagate write and rename failures to the transaction.
Interrupted or truncated DNG frames are ignored during sequence discovery.

Only audio is staged privately for conversion. AudioWriter shares input,
decoding and resampling between mounted WAV audio and finalized FLAC/Opus;
compressed source audio is copied when appropriate. CFA options JSON preserves
bad pixels and sensor geometry and uses the provider's actual output folder
name. Normal clip removal also handles finalization cleanup and auto-resume
state, including removal of the last clip.

The old staged-directory-to-Documents exporter has been removed. Finalization
uses the shared entry sink and the active SAF destination; cancellation calls
`DocumentsContract.deleteDocument` rather than maintaining its own directory
walker. Device checks cover exact output bytes, atomic final names, completed
output retention, nested cancellation cleanup, and retained file/folder grants.
The horizontal settings strip uses ordinary resizable `QScrollArea` behavior;
its geometry and scroll range were compared with the previous subclass using
the real settings panels across portrait and landscape sizes. The custom combo
popup is retained for Android compatibility.

## Build and verification

Use `scripts/build_android_apk.sh` with the SDK, NDK, vcpkg and matching Qt paths
listed in its required environment variables. `scripts/install_qt_android.py`
can install the pinned public Android Qt kit into a chosen directory. The
vcpkg overlay builds unmodified libplacebo; no external-buffer import patch is needed.

Desktop regression targets include `motioncam-lossless-jpeg-test`,
`motioncam-audio-finalize-test` and `motioncam-finalize-interpolation-test`.
GPU checks cover CFA/RGB output, gain/LOG equivalence and resource lifetime;
finalizer checks cover sink failures, sidecars, audio conversion and cancellation.

Gallery controls use a refresh timer coalesced with active video handoffs.
Their occupied bands are packed into a small texture and composited at
viewport pixel coordinates by the native Vulkan presenter. The offscreen
`motioncam-gallery-overlay-test` compares reconstructed overlays against full
widget captures at multiple viewport offsets, sizes, opacities and display
scales, including fractional scaling. Linux compute output uses exportable
storage buffers directly; drivers that reject them retain the transfer-copy
fallback. Set `MOTIONCAM_GALLERY_PERF_FULLSCREEN=1` for actual fullscreen
performance runs (`MOTIONCAM_GALLERY_PERF_VIEWPORT=1280x720` selects a window).
Keep histogram settings identical when comparing runs: raw readback and
histogram computation remain a separate cost.

Android gallery automation accepts `gallery-perf-session`,
`gallery-perf-playback-ms`, `gallery-perf-thumbnails` and `gallery-perf-flags`
intent extras. Use `profile,nohist,hidethumbnails,skipbackfill,preserveproxy,keepalive`
with thumbnails explicitly off for playback comparisons. Basic structured events
are retained in the bounded `files/diagnostics/gallery-perf.log`. Add `zerocopy`
to require resident GPU input and direct shared-image presentation. This strict
validation rejects CPU-decoded DirectLog, full-frame DirectLog readback, host
preview input, buffer-upload presentation and CPU display fallback. Use it with
`nohist`; the raw histogram currently requires CPU samples. Small asynchronous
thumbnail readbacks and Qt control overlays are separate from video-frame
transport and remain supported. Add `capped` to test source-rate pacing; the
default benchmark is uncapped. Add `autofade` to exercise idle fade-out/fade-in
instead of pinning controls, and `presenttrace` to log native frame submissions.
`unfuseddecode` selects the original two-pass
Android decoder for A/B comparisons, and `hashpreview` validates CPU-readable
preview hashes separately from strict zero-copy testing.

To sample CPU call stacks without changing the renderer, use the NDK's
Simpleperf through the gallery runner. Install an optimized, debuggable APK
first, then run:

```sh
python3 scripts/profile_android_gallery.py \
  --ndk "$ANDROID_NDK_ROOT" \
  --session /data/user/0/com.motioncam.fuse/files/sessions/zero-copy-directlog.json \
  --output /tmp/gallery-profile --landscape \
  --symbols build-android-arm64-mediacodec --symbols "$QT_ANDROID_ROOT"
```

The script compares six-second samples near startup and after 16 seconds,
with the thumbnail row open and closed. It keeps source-rate pacing and audio
enabled, disables the histogram, and preserves direct zero-copy video. Reports
include CPU self time, time attributed to called functions, raw samples and
memory snapshots. `--symbols` accepts additional directories containing matching
unstripped libraries. Verbose frame logging is off by default because it adds
measurement overhead; `--frame-log` enables the existing capture/presentation
timers when needed. `--flags guipool` restores Qt's parallel raster fills for
comparison with the Android default of serial drawing, without changing video
decoding or resolution. The script stops the test app afterward and restores
screen rotation and the USB keep-awake setting. CPU samples measure execution, so use
the frame timers alongside them when investigating waits or presentation cadence.

The native overlay atlas uses Qt's ARGB raster format and uploads it as BGRA
on little-endian devices with sampled BGRA support. This avoids repeated channel
swizzling during raster composition; other devices retain an RGBA upload fallback.
The UI stays at native device resolution. Android disables Qt's GUI raster
worker pool: the short fills at high display density were slower when dispatched
to those workers. Other platforms retain their existing Qt setting.

On the CPH2581 landscape test (4096×3072 DirectLog, 30 fps source, audio on,
histogram off), steady measurements after the initial playback burst were:

| Overlay path | Capture time | Displayed fps |
| --- | ---: | ---: |
| RGBA atlas, Qt worker pool | 41.0 ms | 14.2 |
| Native ARGB/BGRA, Qt worker pool | 24.1 ms | 20.1 |
| Native ARGB/BGRA, serial raster drawing | 10.7 ms | 29.0 |
| Same defaults, thumbnail row closed | 7.2 ms | 29.7 |

Capture times are means over the last 80 captures of 35-second runs; displayed
cadence comes from SurfaceFlinger, rather than the application's submission
counter. These are device/clip-specific measurements, not universal limits.

Shared-device textures do not require memory export/import or an external
ownership handoff. If a suitable storage/sampleable texture cannot be created,
ordinary playback retains host output; strict `zerocopy` runs report failure.

`gallery_shared_image` records resident input and direct texture sharing;
`native_present` reports `shared_images`, `buffer_uploads` and `host_uploads`.
Only the one-pixel startup placeholder is excluded from video upload counts.
The shared-image path allocates its compute destination directly, without an
unused full-frame scratch buffer. On the tested Adreno device the MediaCodec
AHardwareBuffer and retained tiled-JPEG decode paths both pass this contract.
Color conversion, gain/LOG processing and swapchain rendering still run on the
GPU; zero-copy here means no full-frame CPU round trip or presenter upload,
not an absence of GPU processing. Also check background/resume and process
exit-info on a connected device.

Other drivers and non-seekable document providers still need device coverage.
Android starts with a 256 MiB frame cache, two I/O workers and at most four
processing workers. Raising these budgets requires memory-pressure measurements.

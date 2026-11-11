# Android arm64 notes

## Distribution and use

The `android-apk` workflow builds a single `arm64-v8a` APK with Qt 6.12.0 and
the same pinned vcpkg dependency snapshot as the desktop jobs. The package is
built in Release configuration and signed with a local debug key for sideloading
from a GitHub Actions artifact. The signing key does not enable debugging.
It is not a stable upgrade identity across different builders; a public release
needs a persistent release keystore. Supply `ANDROID_SIGNING_KEYSTORE`,
`ANDROID_SIGNING_ALIAS`, `ANDROID_SIGNING_STORE_PASSWORD`, and
`ANDROID_SIGNING_KEY_PASSWORD` to the signing helper.

Release APKs retain `--gallery-perf-session` benchmarking and its scenario
controls. Detailed profiling, diagnostic file sinks, GPU validation/hash checks,
and developer environment overrides require a separate Debug native build;
they are ignored by delivered Release APKs. The build script and automated
jobs compile only Release application code and Release vcpkg dependencies.

Android cannot expose app-owned FUSE mounts. The `ProcessingFileSystem`
backend keeps imported clips in-process for metadata, gallery previews, and
finalization. The Android **Import clip** action uses the system picker. For a
seekable document provider, readers use descriptors backed by the retained
content URI. MCRAW, video/audio, DNG frames and calibration
files are read through these descriptors, including seeking. Providers that
cannot supply seekable input report an error; there is no private-copy fallback.
DNG sequences use the document-tree folder picker. Private import directories
contain only empty filename index entries; URI, size and revision metadata are
stored with the retained grants. No imported document bytes are stored there.
A bounded cache retains URI descriptors for reuse; positional reads give each
reader an independent cursor. Descriptor lookup resolves URI, size and revision
in one Java call; DNG playback reuses the frame size validated during import.
Revision checks remain live on each open. Cached handles are released when their clip is
unmounted. Bounded lookahead overlaps opens during sequential playback. Reimporting an
already mounted document or sequence is ignored.
Session JSON selected through the picker is also read from its URI; the small
automatic resume snapshot remains application state.

Sidecars are refreshed automatically during manual import and session load,
using retained folder permissions. Manual clip import requests missing source
folder access automatically; canceling leaves the clips imported. Session load
uses existing grants without opening permission prompts.
**Grant source folder access / rescan sidecars** can grant access later or
refresh an open session. Choose a parent folder to cover its clips and nested
source folders with one grant. The system picker starts at the clip's source
folder when available. Directory listings and permission lists are shared for
one import/scan operation and discarded afterward, so later refreshes still
observe added, changed and removed sidecars. DNG folders index documents
and URI metadata in one pass, without per-frame sidecar scans. Import progress
is shown while indexing and preparing sources. Provider queries and decoding
run in workers, and progress overlays follow window resizing. Sidecars include
calibration JSON, Gyroflow JSON, GCSV, PP3, XML, and other matching sidecars,
and relative files named by `dng_white`, `dng_opcode`, `dcp`, and `gyroflow`.
These remain at their original URIs. Refreshing reindexes the source rather
than copying it. Gallery thumbnails have a 32 MiB memory cache and, on Android,
a transient disk cache limited to 64 MiB of JPEG data. Generated timeline
and analysis files share a separate 64 MiB disk budget on Android and desktop,
evicting oldest entries. Missing private sequence indexes are rebuilt using the
stored source identity and retained grant when restoring a session.
Supported tiled lossless-JPEG DNG previews keep decoded pixels resident on Vulkan
through proxy reduction, color processing and shared-image presentation. Bayer
proxy reduction preserves CFA phase; deferred gains sample the original sensor
coordinates. RGB proxies use the same nearest-neighbor samples as the CPU path.
Unsupported layouts or operations retain the CPU fallback. With histogram and
thumbnail row disabled, `zerocopy,preserveproxy` enforces resident proxy input and
shared-image presentation; `dng_gpu_jpeg_resident` records source/output dimensions
and scale.

DNG indexing reads compact TIFF directories and tag values rather than allocating
full image buffers. Their persistent metadata indexes use the original document
URI, size and revision to invalidate changed frames. Providers without a usable
revision keep reading source metadata directly. Calibration sidecars are
refreshed independently. **Preferences → Indexing Cache → Clear** removes these
DNG metadata indexes along with the existing timeline and analysis caches.
**Create JSON** writes the per-clip file to the granted source directory and
refreshes the URI index. If that clip has no linked source directory,
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
app-private copy of the whole finalized sequence. Camera Native finalization uses
linked FFmpeg libraries for LLOG60 conversion, video encoding, and audio muxing;
it does not launch an FFmpeg executable. ProRes and CineForm use the linked
software encoders. Android-only device encoder formats include `H264 8b 100`,
`HEVC 10b 100`, and `AV1 10b 100`. The second field selects 8 or 10 bits;
the last number is the requested bitrate in Mbps, defaulting to 100 when omitted.
The presets use 100 Mbps. Older saved formats are normalized to the shorter labels. Selection checks hardware
acceleration, input layout, dimensions, frame rate, bitrate, and the 10-bit
profile when requested. It never falls back to software HEVC/AV1 or reduces
the requested bit depth. Dimensions currently must be divisible by 16.
8-bit input uses FFmpeg's MediaCodec wrapper with NV12 or planar YUV420.
10-bit input uses a direct NDK MediaCodec adapter with P010 on Android 13+
(API 33+), requiring the device to advertise both P010 input and the matching
10-bit codec profile. Devices offering 10-bit encoding only through surfaces
are not supported by this byte-buffer path. H.264 High 10 is included only
when the device advertises it; HEVC Main 10 and AV1 Main 10 are likewise
capability-dependent. Software HEVC/AV1 and AV1 synth-noise modes remain
disabled in Android's dependency build.

H.264 and HEVC use MOV when copying uncompressed audio. AV1 uses MP4 and
converts WAV audio to lossless FLAC when no audio codec suffix is requested.
An explicit FLAC/Opus suffix or existing compressed audio selects MP4.

Color conversion explicitly uses full data levels, and device encoder output
is rejected when its reported range, bit depth, chroma, dimensions, or BT.2020
matrix/primaries differ. No deprecated `yuvj*` pixel format is requested for
encoding. FFmpeg may still report `yuvj420p` when *decoding* a full-range 8-bit
bitstream; it is a legacy representation, not a separate encoded format.
ProRes/MOV's older nclc tag has no range flag: the pixels remain full-range,
and the native JSON sidecar declares `dataLevels: Full`. A temporary seekable MOV/MP4 and
its sidecars are staged privately,
then copied in bounded chunks directly into the selected Android destination.
Only DNG sequences create a per-clip subfolder. Native video and metadata-only
exports reserve a shared basename for their files; repeat exports add a numeric
suffix instead of overwriting existing files. Video names retain `_LLOG60_NATIVE`
without a codec suffix. Cancelling a direct export removes only its newly
created documents, never the destination directory or pre-existing files.
Temporary files are removed on completion or failure. Display matrices use the
same clockwise orientation as the importer, preserving 90° and 270° rotations. 7z mode still requires an external executable.

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
with avfilter support so Native Video's Vulkan conversion and gallery color
processing can run on compatible devices. Android uses the native Vulkan
gallery presenter inside the existing Qt activity surface. The same presenter is
selected by default on supported desktop builds. Gallery frames are presented
by its Vulkan QWindow and swapchain; QWidget only hosts the window and controls.
Android does not use a QWidget image-painting fallback. Qt backing-store backend
settings are scoped to Android and do not override desktop backend selection.
Decoding, gallery processing and native presentation can share one
Vulkan device, with separate libplacebo command recorders and common queue
locks. The presenter wraps the processed VkImage directly, with an in-device
timeline semaphore for readiness. There are no exported memory descriptors or
inter-device semaphore transfers. Unsupported image output retains the host
upload fallback.
This transport is platform-independent. It is selected automatically on
unified-memory GPUs; Android and Apple Silicon assume unified memory, while
other devices are classified by their Vulkan memory properties. Linux retains
its tested DMA-BUF transport for separate devices. DMA-BUF availability is
independent of whether the GPU has dedicated memory. Desktop presentation
retries through its existing backend if native Vulkan fails.

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
platform adapters, independent of that memory policy. macOS is enabled but requires validation
on hardware; Android testing does not validate its import/presentation backend.

Native Video uses MediaCodec AImageReader output when FFmpeg Vulkan decode is
unavailable. Hardware-buffer imports and submission resources are cached, with
at most three output images. The intermediate is RGBA16 UNORM; displayed 10-bit
frames use packed RGB10A2 UNORM. Full-resolution deferred RGB decoding fuses
HLG/LOG60/LLOG60 linearization into hardware-buffer sampling, so the gallery consumes
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

Run `bash scripts/build_android_apk.sh` from the repository root. Its default
output is `build-android-arm64/android-build-release.apk`;
an optional first argument selects another build directory.

Copy `scripts/android-build.env.example` to the ignored `android-build.env`
and adjust its paths for a persistent local setup. The script loads it
automatically; CI supplies the same variables through its environment.
Use matching Qt 6.12.0 Android and Linux host kits, a full JDK 21, SDK
platform/build tools 36, NDK 27.2.12479018, and the pinned vcpkg checkout.
Keep these tools outside `/tmp`. The example uses `~/Qt/6.12.0` and
`~/.local/share/rawpreamp`; generated build files and dependencies stay in
`build-android-arm64`. `scripts/install_qt_android.py` installs the Android
kit; aqt can install the matching host kit with Qt Declarative and Shader Tools.

`CMAKE_BUILD_PARALLEL_LEVEL` and `VCPKG_MAX_CONCURRENCY` override the default
four build jobs. The build requires the Vulkan backend and retains runtime
CPU fallback. The vcpkg overlay builds unmodified libplacebo; no external-buffer
import patch is needed.

Desktop regression targets include `rawpreamp-lossless-jpeg-test`,
`rawpreamp-audio-finalize-test` and `rawpreamp-finalize-interpolation-test`.
GPU checks cover CFA/RGB output, gain/LOG equivalence and resource lifetime;
finalizer checks cover sink failures, sidecars, audio conversion and cancellation.

Gallery controls use a refresh timer coalesced with active video handoffs.
Their occupied bands are packed into a small texture and composited at
viewport pixel coordinates by the native Vulkan presenter. The offscreen
`rawpreamp-gallery-overlay-test` compares reconstructed overlays against full
widget captures at multiple viewport offsets, sizes, opacities and display
scales, including fractional scaling. Linux compute output uses exportable
storage buffers directly; drivers that reject them retain the transfer-copy
fallback. Set `RAWPREAMP_GALLERY_PERF_FULLSCREEN=1` for actual fullscreen
performance runs (`RAWPREAMP_GALLERY_PERF_VIEWPORT=1280x720` selects a window).
Keep histogram settings identical when comparing runs: raw readback and
histogram computation remain a separate cost.

Import-only timing uses `--gallery-perf-thumbnails off --gallery-perf-flags importonly`
on desktop, or the equivalent Android intent extras. The `session_import_complete`
event covers source preparation and mounting without playback. Add `sidecarscan`
on Android to time a separate explicit rescan (`sidecar_scan_complete`); this may
open the folder picker if grants are missing. Repeat the import to measure a
populated DNG index cache, or clear **Preferences → Indexing Cache** to measure
rebuilding it. `noindexcache` bypasses the persistent DNG metadata cache for
controlled comparisons without deleting it. `profile` also reports `dng_source_read`
open, allocation and read timings. `preserveproxy` retains the session's saved
proxy scale and reconstruction quality.

Android gallery automation accepts `gallery-perf-session`,
`gallery-perf-playback-ms`, `gallery-perf-thumbnails` and `gallery-perf-flags`
intent extras. Use `profile,nohist,hidethumbnails,skipbackfill,preserveproxy,keepalive`
with thumbnails explicitly off for playback comparisons. Basic structured events
are retained in the bounded `files/diagnostics/gallery-perf.log`. Add `zerocopy`
to require resident GPU input and direct shared-image presentation. This strict
validation rejects CPU-decoded Native Video, full-frame Native Video readback, host
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
  --session /data/user/0/com.rawpreamp.app/files/sessions/zero-copy-native-video.json \
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

On the CPH2581 landscape test (4096×3072 Native Video, 30 fps source, audio on,
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

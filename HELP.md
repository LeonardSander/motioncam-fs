# MotionCam FS Help

## Quickstart
- Windows: Put your source `.mcraw` files on an NTFS volume (required).
- macOS: No NTFS requirement. Mounts are virtual folders.
- Open MotionCam FS, click **Open** or drag in `.mcraw` files to mount.
- Adjust settings as needed (global on the right). Use **Apply to Selected** for overrides or **Apply to All** for everything.
- Mounted DNG sequences appear in the mount folder (Windows default: source folder; macOS default: `~/Mounts/MotionCamFuse`).
- Use **Play** to open the mounted sequence in MotionCamPlayer (set path in Preferences).

## Workspace Tour
- **Mount List**: Each card shows the mounted file, status, FPS, and actions (Play, Open folder, Remove). A local badge appears when per-file overrides are active.
- **Settings Pane**: Global render settings (Draft, Vignette, CFR, Crop, Exposure, Quad Bayer, etc.).
- **Apply Buttons**:
  - **Apply to Selected**: push current settings to highlighted mounts (sets local overrides).
  - **Apply to All**: push current settings globally and clear local overrides.
- **Status Bar**: Refresh progress, FPS updates, cache cleanup events.

## Preferences
- **DNG Output Folder / Mount Folder**: Windows - leave empty to write next to source (must be NTFS) or set a dedicated NTFS folder. macOS - leave empty to use the default mount root (`~/Mounts/MotionCamFuse`) or set a custom mount folder.
- **Delete on Unmount**: Windows - remove materialized DNGs when unmounting/clearing. macOS - remove the empty mount folder under the mount root.
- **Video Player**: Path to MotionCamPlayer.exe (Windows) or MCRAW_Player.app (macOS) for the Play action.
- **Unique Camera Model**: Configure the camera-model metadata override in the main settings panel.
- **Reset**: Clears the saved mount/output folder and player path.
- **Cache Management**: Windows only. Mode (Off/Quota), quota (GB), cleanup interval (sec).

## Render Settings (Right Panel)
- **Draft Mode Quality**: Downscale for speed (2×/4×/8×). High-quality first frame is always on in proxy mode.
- **Frame Rate Conversion**: Defaults to Prefer Drop Frame; choose another preset or a custom value as needed.
- **Crop Target**: Enable cropping and choose a target ratio/size.
- **Vignette Correction**: Enabled by default to bake the lens shading fix. “Vignette Only Color” is also enabled by default.
- **Scale Raw**: Enable to normalize the shading map.
- **Normalize Exposure**: Enabled by default; it can increase load time.
- **Exposure Compensation**: Adjust white point/gain (combo box).
- **Quad Bayer**: Interpret as Quad Bayer (off by default).

## Apply Logic
- Global changes (from the right pane) auto-apply to all when you click **Apply to All** or when auto-apply triggers. Local overrides stay per mount until cleared.
- Per-file overrides: select cards and use **Apply to Selected**.
- Reset a clip's local settings from its clip card to return it to the global settings.

## Troubleshooting
- **Mount failed / NTFS required (Windows)**: Ensure source and/or output folder is NTFS. Set a custom NTFS output folder in Preferences if needed.
- **Mount failed (macOS)**: Check macFUSE approval and ensure the mount folder is writable and empty.
- **Audio missing**: Ensure the source has audio; re-mount if you changed audio settings.
- **dng_validate errors**: Try disabling Scale Raw; ensure the input files are valid.
- **Cache full**: Increase quota or clear mounts; “Delete on Unmount” removes generated DNGs.
- **Slow loads**: Disable Normalize Exposure, use Draft quality, or reduce cache cleanup frequency.

## Performance Tips
- Use Draft quality + proxy defaults for quick browsing; switch to full quality when exporting.
- Keep mounts on a fast local SSD (NTFS on Windows, APFS/HFS+ on macOS); set a quota that fits your drive.
- Only enable Normalize Exposure when you see exposure flicker; it adds processing time.

## Universal macOS Build
- Use a universal binary for a single app that runs on Intel and Apple Silicon.
- Build script: `scripts/build_macos_universal.sh` (requires `VCPKG_ROOT`).
  - Example:
    - `export VCPKG_ROOT=/path/to/vcpkg`
    - `./scripts/build_macos_universal.sh`
  - Output: `build-universal-vcpkg/MotionCamFuse.app`

## Shortcuts & Actions
- **Select All**: Ctrl/Cmd+A in the mount list.
- **Reset buttons**: Normalize Exposure reset, CFR reset per-file/global.
- Card actions: Play, open mounted folder, remove, toggle selection.

## Release Notes
- Keep a simple changelog per build (features, fixes, known issues) so users know what changed.

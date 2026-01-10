# MotionCam FS Help

## Quickstart
- Put your source `.mcraw` files on an NTFS volume (required on Windows).
- Open MotionCam FS, click **Open** or drag in `.mcraw` files to mount.
- Adjust settings as needed (global on the right). Use **Apply to Selected** for overrides or **Apply to All** for everything.
- Mounted DNG sequences appear in the output folder (defaults to source folder, or set a custom NTFS folder in Preferences).
- Use **Play** to open the mounted sequence in MotionCamPlayer (set path in Preferences).

## Workspace Tour
- **Mount List**: Each card shows the mounted file, status, FPS, and actions (Play, Open folder, Remove). A local badge appears when per-file overrides are active.
- **Settings Pane**: Global render settings (Draft, Vignette, CFR, Crop, Exposure, Quad Bayer, etc.). Resets exist for Normalize Exposure and Frame Rate Conversion.
- **Apply Buttons**: 
  - **Apply to Selected**: push current settings to highlighted mounts (sets local overrides).
  - **Apply to All**: push current settings globally and clear local overrides.
- **Status Bar**: Refresh progress, FPS updates, cache cleanup events.

## Preferences
- **DNG Output Folder**: Leave empty to write next to source (must be NTFS). Set a dedicated NTFS folder if your source isn’t NTFS.
- **Delete on Unmount**: Also remove generated DNGs when unmounting/clearing.
- **Video Player**: Path to MotionCamPlayer.exe for the Play action.
- **Unique Camera Model**: Overrides camera model metadata.
- **Cache Management**: Mode (Off/Quota), quota (GB), cleanup interval (sec).

## Render Settings (Right Panel)
- **Draft Mode Quality**: Downscale for speed (2×/4×/8×). High-quality first frame is always on in proxy mode.
- **Frame Rate Conversion**: Disable by default. Enable to conform using presets/custom value. Reset button restores defaults.
- **Crop Target**: Enable cropping and choose a target ratio/size.
- **Vignette Correction**: Disabled by default; enable to bake lens shading fix. “Vignette Only Color” is disabled by default.
- **Scale Raw**: Enable to normalize shading map (off by default).
- **Normalize Exposure**: Off by default; use only when auto exposure shows irregularities. It can increase load time. Reset button clears per-file normalization.
- **Exposure Compensation**: Adjust white point/gain (combo box).
- **Quad Bayer**: Interpret as Quad Bayer (off by default).

## Apply Logic
- Global changes (from the right pane) auto-apply to all when you click **Apply to All** or when auto-apply triggers. Local overrides stay per mount until cleared.
- Per-file overrides: select cards and use **Apply to Selected**.
- Reset buttons remove local normalize exposure/CFR overrides on selected mounts.

## Troubleshooting
- **Mount failed / NTFS required**: Ensure source and/or output folder is NTFS (Windows). Set a custom NTFS output folder in Preferences if needed.
- **Audio missing**: Ensure the source has audio; re-mount if you changed audio settings.
- **dng_validate errors**: Try disabling Scale Raw; ensure the input files are valid.
- **Cache full**: Increase quota or clear mounts; “Delete on Unmount” removes generated DNGs.
- **Slow loads**: Disable Normalize Exposure, use Draft quality, or reduce cache cleanup frequency.

## Performance Tips
- Use Draft quality + proxy defaults for quick browsing; switch to full quality when exporting.
- Keep DNG output on a fast NTFS SSD; set a quota that fits your drive.
- Only enable Normalize Exposure when you see exposure flicker; it adds processing time.

## Shortcuts & Actions
- **Select All**: Ctrl/Cmd+A in the mount list.
- **Reset buttons**: Normalize Exposure reset, CFR reset per-file/global.
- Card actions: Play, open mounted folder, remove, toggle selection.

## Release Notes
- Keep a simple changelog per build (features, fixes, known issues) so users know what changed.

# MotionCam Fuse – Virtual File System

**Work in Progress**

**MotionCam Fuse** allows mounting MCRAW files (proprietary MotionCam Raw Capture Format) as projected folders containing DNG sequences. This enables a convenient raw video editing workflow—preferably in **Davinci Resolve**.

<img width="400" height="553" alt="mcfs_screenshot" src="https://github.com/user-attachments/assets/d702885d-a24f-4444-8c0f-2104c7a016f8" />

---

### Key Features

In addition to the features of the original repository, this fork adds:

- **Reducing Vignette Correction to Color Adjustments**  
  Included Vignette correction metadata is modified before being applied to the image data to retain natural vignetting and dynamic range in image corners.

- **Exposure Normalization**  
  Exposure changes between frames are normalized based on per-frame ISO and shutter speed metadata to avoid exposure transitions. This functionality relies on the per-frame **Baseline Exposure** DNG tag, which is recognized by Davinci Resolve.

- **Improved Framerate Handling**  
  Since the underlying raw video streams captured in MCRAW files often feature a non standard and possibly variable framerate, this fork expands upon the logic to determine a suitable framerate target for constant-framerate conversion.

---

### Platform Support

Currently, **only Windows builds** are maintained in this fork.

⚠️ **Note:** Expect slowdowns when opening MCRAW files or changing settings.

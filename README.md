# MotionCam Fuse - Virtual File System

Work in progress

MotionCam Fuse allows mounting MCRAW files (propriatary MotionCam Raw Capture Format) to projected Folders of DNG sequences. This allows for a convenient Raw Video editing workflow preferably in Davinci Resolve.

<img width="811" height="1120" alt="image" src="https://github.com/user-attachments/assets/7d61ecad-8397-4b9c-a044-c5c018a03867" />

Additionally to the features of the source repository, this fork adds functionality to reduce the included vignette correction to be reduced to color corrections before being applied to the image data. 

Furthermore based on per frame ISO and shutter speed metadata, exposure settings changes can be normalized to avoid exposure transients. This relies on the per frame Baseline Exposure DNG Tag which is recognized by Davinci Resolve. 

Since the underlying raw video streams captured in MCRAW files often feature a non standard and possibly variable framerate, the logic to determine a suitable target framerate for constant framerate conversion is expanded upon in this fork. 


For this fork only Windows builds are being focussed on. Expect slowdowns upon opening MCRAW files and changing settings.



# MotionCam Fuse – Virtual File System

**Work in Progress**

**MotionCam Fuse** allows mounting MCRAW files (proprietary MotionCam Raw Capture Format) as projected folders containing DNG sequences. This enables a convenient raw video editing workflow—preferably in **Davinci Resolve**.

<img width="800" height="1105" alt="mcfs_screenshot" src="https://github.com/user-attachments/assets/d702885d-a24f-4444-8c0f-2104c7a016f8" />

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

# MotionCam Fuse – Virtual File System

> **Work in Progress**

**MotionCam Fuse** allows mounting MCRAW files (proprietary MotionCam Raw Capture Format) as projected folders containing DNG sequences. This enables a convenient raw video editing workflow—preferably in **Davinci Resolve**.

---

<img width="1531" height="724" alt="image" src="https://github.com/user-attachments/assets/880479b3-142f-4a54-9ce1-bf63b59b6c5c" />

---

Upon mounting of the MCRAW files, their corresponding folders will appear in the same directory as the MCRAWs if not defined otherwise. For Windows the Output Folder needs to be defined on a NTFS drive and in general only use Fuse with SSD storage. 

At first these DNG files are only projected and do not consume storage space. As soon as the individual files are accessed, their corresponding frames are read from MCRAW, potentially preprocessed and written to storage. To cache all files without manually accessing each one use [this script](https://discord.com/channels/980884979955421255/1377309561219973121/1423424334185234574). Keep in mind that the cached files will be overriden dynamically with projected ones again if Fuse settings are altered.

These cached files will remain in storage for now when unmounting MCRAW or closing Fuse. If Fuse is not active, projected files remain visible in the File Explorer and will appear empty if opened. To access these files just open Fuse again for the previously saved session to be restored. The cache can be manually cleared by closing Fuse, deleting folder contents and folders afterwards. These will appear again when Fuse is opened once again and the session gets restored.

---

### Features

[Showcase Video](https://youtu.be/knACG5jy-rk)

- **Improved Framerate Handling**

  Since the underlying raw video streams captured in MCRAW files often feature a non-standard and possibly variable frame rate, a conversion to a constant standard frame rate becomes necessary for delivery with real time playback. Fuse chooses a suitable target delivery frame rate based on the median frame rate of a given MCRAW clip. For non-real time playback or gyroflow usage the median frame rate can be chosen as target as well.  

- **Exposure Normalization**
   
  Exposure changes between frames are compensated for to eliminate exposure transitions in the video. This functionality relies on the per-frame **Baseline Exposure** DNG tag, which is recognized by Davinci Resolve. A suitable exposure compensation value per frame is determined by the shutter speed and ISO settings utilized per frame in a MCRAW clip. Additionally a static exposure compensation can be chosen in the corresponding combobox (Blackmagic Camera Model will stop it from working in DaVinci Resolve).

- **Override Data Levels**
  
  White and Black Levels used in Fuse will default to their dynamic tags stored in MCRAW. Static tags are also still available to choose as a fallback option (choose that to apply levels override from calibration.json).

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
  
  This mode reduces the resolution of the raw image by discarding pixel values. Performance is prioritised and heavy aliasing is introduced to the image. Only use while editing and turn off for delivery. However if MCRAWs contain image data with a quad bayer cfa, the 2x binning option will sum 2by2 pixels to return a binned bayer image. This operation also results in an increase of precision per summed pixel (10b to 12b).

- **Off Center Cropping**
  
  16:9 sensor modes are commonly used by modern devices when 60fps capture is requested. However many of these devices do not provide the suitable raw output configuration which results in an captured image with a buffer underflow. The empty data recorded in the bottom part of the image can be conveniently cropped out using this option. This is the only way to have properly aligned vignette correction on a capture like this. Only full sensor captures without cropping are compatible (Also reselect lens when choosing 60fps slot to not capture junk data in underflown image area).

- **Quad Bayer CFA Support**
  
  Unbinned quad bayer cfa footage requires modified camera drivers to be captured if not for Pixel phones. These captures are identified as such if 'Enable Remosaic' was enabled during capture or if the 'Interpret as QBCFA' checkbox is checked in Fuse. So far Fuse is able to apply vignette correction and the log transfer curve to both the unbinned data or after it is binned via the 2x binning option mentioned above. If left unbinned by default DNGs will still report a normal bayer cfa and will be misinterpreted. Defining the proper 4by4 QBCFA in DNG metadata is possible as well, but compatability will vary. RawTherapee crashes upon opening QBCFA DNGs for example. Further treatment options like Quad Bayer Demosaic and Remosaic are planned. The latter is necessary as DaVinci Resolve does not support demosaiced DNGs. 

---

### Platform Support

Currently, **only Windows builds** are up to date with the presented functionality. For now to install the current version it is required to use an older Fuse installer first like mentioned [here](https://discord.com/channels/980884979955421255/1377309561219973121/1418033196762665093). 

For [Mac](https://discord.com/channels/980884979955421255/981049638079582208/1416131149528432672) and [Linux](https://discord.com/channels/980884979955421255/1377309561219973121/1390627443550851102) only older Fuse builds are available.

[Changelog](https://discord.com/channels/980884979955421255/1377309561219973121/1420903594198040717) in reply chain

If these links do not open you need to join the [MotionCam Discord Community](https://discord.gg/Vy4gQNEdNS) first.

⚠️ **Note:** Expect slowdowns when opening MCRAW files or changing settings.

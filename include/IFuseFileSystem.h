#pragma once

#include <string>
#include <optional>

#include "Types.h"
#include "RenderConfig.h"

namespace motioncam {

using MountId = int;

constexpr auto InvalidMountId = -1;

struct FileInfo {
    float medFps;
    float avgFps;
    float fps;
    int totalFrames;
    int droppedFrames;
    int duplicatedFrames;
    int width;
    int height;
    std::string dataType;        // "Bayer CFA", "Quad Bayer CFA", or "RGB"
    std::string levelsInfo;      // e.g., "1023/64 -> 1023/0 RAW10"
    float runtimeSeconds;        // Runtime in seconds based on audio track
};

class IFuseFileSystem {
public:
    virtual ~IFuseFileSystem() = default;

    IFuseFileSystem(const IFuseFileSystem&) = delete;
    IFuseFileSystem& operator=(const IFuseFileSystem&) = delete;

    virtual MountId mount(const RenderConfig& config, const std::string& srcFile, const std::string& dstPath) = 0;
    virtual void unmount(MountId mountId) = 0;
    virtual void updateOptions(MountId mountId, const RenderConfig& config) = 0;
    virtual std::optional<FileInfo> getFileInfo(MountId mountId) = 0;

protected:
    IFuseFileSystem() = default;
};

} // namespace motioncam

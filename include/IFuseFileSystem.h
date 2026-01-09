#pragma once

#include <string>
#include <optional>


#include "Types.h"

namespace motioncam {

using MountId = int;

constexpr auto InvalidMountId = -1;

enum class CachePolicy {
    Off,
    Quota
};

struct FileInfo {
    float medFps;
    float avgFps;
    float fps;
    int totalFrames;
    int droppedFrames;
    int duplicatedFrames;
    int width;
    int height;
};

class IFuseFileSystem {
public:
    virtual ~IFuseFileSystem() = default;

    IFuseFileSystem(const IFuseFileSystem&) = delete;
    IFuseFileSystem& operator=(const IFuseFileSystem&) = delete;

    virtual MountId mount(const RenderSettings& settings, const std::string& srcFile, const std::string& dstPath) = 0;
    virtual void unmount(MountId mountId) = 0;
    virtual void updateOptions(MountId mountId, const RenderSettings& settings) = 0;
    virtual std::optional<FileInfo> getFileInfo(MountId mountId) = 0;
    virtual bool generateThumbnail(MountId mountId, const std::string& outputPath, int width = 320, int height = 240) = 0;
    virtual void setCachePolicy(CachePolicy policy) = 0;
    virtual void setCacheQuotaBytes(std::uint64_t bytes) = 0;
    virtual void cleanupCacheExpired() = 0;

protected:
    IFuseFileSystem() = default;
};

} // namespace motioncam

#pragma once

#include <string>
#include <optional>
#include <functional>
#include <cstdint>

#include "Types.h"
#include "DNGImage.h"

namespace motioncam {

struct FileInfo;

using MountId = int;

constexpr auto InvalidMountId = -1;

enum class CachePolicy {
    Off,
    Quota
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
    virtual void setCachePolicy(CachePolicy) {}
    virtual void setCacheQuotaBytes(std::uint64_t) {}
    virtual void cleanupCacheExpired() {}
    virtual void finalize(
        MountId mountId,
        const std::string& destination,
        bool jpegCompression,
        const FinalizeOptions& options,
        const std::function<bool(size_t, size_t, const std::string&)>& progress,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady = {},
        bool writeFiles = true) = 0;
    // Render with an isolated virtual filesystem. Preview settings must never
    // mutate the mounted projection or invalidate/re-estimate its entries.
    virtual void renderPreview(
        MountId mountId, const RenderSettings& settings,
        const PreviewOptions& options,
        const std::function<bool(size_t, size_t, const std::string&)>& progress,
        const std::function<void(PreviewFrame&&)>& frameReady) = 0;

protected:
    IFuseFileSystem() = default;
};

} // namespace motioncam

#pragma once

#include <string>
#include <optional>
#include <functional>

#include "Types.h"

namespace motioncam {

struct FileInfo;

using MountId = int;

constexpr auto InvalidMountId = -1;

class IFuseFileSystem {
public:
    virtual ~IFuseFileSystem() = default;

    IFuseFileSystem(const IFuseFileSystem&) = delete;
    IFuseFileSystem& operator=(const IFuseFileSystem&) = delete;

    virtual MountId mount(const RenderSettings& settings, const std::string& srcFile, const std::string& dstPath) = 0;
    virtual void unmount(MountId mountId) = 0;
    virtual void updateOptions(MountId mountId, const RenderSettings& settings) = 0;
    virtual std::optional<FileInfo> getFileInfo(MountId mountId) = 0;
    virtual void finalize(
        MountId mountId,
        const std::string& destination,
        bool jpegCompression,
        const FinalizeOptions& options,
        const std::function<bool(size_t, size_t, const std::string&)>& progress) = 0;

protected:
    IFuseFileSystem() = default;
};

} // namespace motioncam

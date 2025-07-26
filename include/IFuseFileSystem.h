#pragma once

#include <string>

#include "Types.h"

namespace motioncam {

using MountId = int;

constexpr auto InvalidMountId = -1;

class IFuseFileSystem {
public:
    virtual ~IFuseFileSystem() = default;

    IFuseFileSystem(const IFuseFileSystem&) = delete;
    IFuseFileSystem& operator=(const IFuseFileSystem&) = delete;

    virtual MountId mount(FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget, const std::string& srcFile, const std::string& dstPath) = 0;
    virtual void unmount(MountId mountId) = 0;
    virtual void updateOptions(MountId mountId, FileRenderOptions options, int draftScale, const std::string cfrTarget, const std::string cropTarget) = 0;
    virtual float getFps(MountId mountId) const = 0;

protected:
    IFuseFileSystem() = default;
};

} // namespace motioncam

#pragma once

#include "Types.h"
#include "IFuseFileSystem.h"
#include "RenderConfig.h"

#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace motioncam {

class IVirtualFileSystem {
public:
    virtual ~IVirtualFileSystem() = default;

    IVirtualFileSystem(const IVirtualFileSystem&) = delete;
    IVirtualFileSystem& operator=(const IVirtualFileSystem&) = delete;

    virtual std::vector<Entry> listFiles(const std::string& filter) const = 0;
    virtual std::optional<Entry> findEntry(const std::string& fullPath) const = 0;
    virtual int readFile(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async) = 0;

    virtual void updateOptions(const RenderConfig& config) = 0;
    virtual FileInfo getFileInfo() const = 0;

protected:
    IVirtualFileSystem() = default;
};

}

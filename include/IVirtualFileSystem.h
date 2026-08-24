#pragma once

#include "Types.h"

#include <optional>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace motioncam {

struct FileInfo;

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

    // Produce the complete bytes for an entry. Mounted range reads and
    // permanent exports must share this path so they cannot render different
    // frame sequences or metadata.
    virtual std::shared_ptr<std::vector<char>> materializeFile(
        const Entry& entry,
        bool jpegCompression = false) = 0;

    virtual void updateOptions(const RenderSettings& settings) = 0;
    virtual FileInfo getFileInfo() const = 0;
    virtual bool generateThumbnail(const std::string&, int, int) { return false; }

    // Finalizers may ask whether two output entries refer to source frames with
    // identical encoded image data. Non-DNG implementations have no such
    // source payloads.
    virtual bool sourceImagePayloadsEqual(const Entry&, const Entry&) { return false; }

protected:
    IVirtualFileSystem() = default;
};

}

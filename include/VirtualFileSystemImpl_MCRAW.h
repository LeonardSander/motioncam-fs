#pragma once

#include <IVirtualFileSystem.h>

namespace BS {
class thread_pool;
}

namespace motioncam {

class Decoder;
class LRUCache;

class VirtualFileSystemImpl_MCRAW : public IVirtualFileSystem
{
public:
    VirtualFileSystemImpl_MCRAW(FileRenderOptions options, int draftScale, const std::string& file);

    std::vector<Entry> listFiles(const std::string& filter = "") const override;
    std::optional<Entry> findEntry(const std::string& fullPath) const override;
    size_t readFile(
        const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const override;
    void updateOptions(FileRenderOptions options, int draftScale) override;

private:
    void init(FileRenderOptions options);

    size_t generateFrame(
        const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const;

    size_t generateAudio(
        const Entry& entry, FileRenderOptions options, const size_t pos, const size_t len, void* dst, std::function<void(size_t, int)> result) const;

private:
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
    std::vector<uint8_t> mAudioFile;
    int mDraftScale;
    float mFps;
    double mBaselineExpValue;
};

} // namespace motioncam

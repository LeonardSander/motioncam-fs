#include "win/FuseFileSystemImpl_Win.h"
#include "win/dirInfo.h"
#include "win/virtualizationInstance.h"

#include "VirtualFileSystemImpl_MCRAW.h"
#include "LRUCache.h"

#include <iostream>
#include <ntstatus.h>
#include <mutex>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/locale.hpp>

#include <BS_thread_pool.hpp>

// Logging
#include <spdlog/spdlog.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/msvc_sink.h>

namespace fs = boost::filesystem;
namespace lcv = boost::locale::conv;

namespace motioncam {

constexpr auto CACHE_SIZE = 128 * 1024 * 1024; // Small cache size as we write the files to disk
constexpr auto IO_THREADS = 4;

namespace {

    inline std::wstring fromUTF8(const std::string& s)
    {
        return lcv::utf_to_utf<wchar_t>(s);
    }

    inline std::string toUTF8(const std::wstring& ws)
    {
        return lcv::utf_to_utf<char>(ws);
    }

    inline std::string toUTF8(PCWSTR ws)
    {
        return lcv::utf_to_utf<char>(std::wstring(ws == nullptr ? L"" : ws));
    }

    void updatePlaceHolder(PRJ_PLACEHOLDER_INFO& placeholderInfo, const Entry& entry, const FileRenderOptions options, int draftScale) {
        placeholderInfo.FileBasicInfo.IsDirectory = entry.type == EntryType::DIRECTORY_ENTRY;
        placeholderInfo.FileBasicInfo.FileSize = entry.size;
        placeholderInfo.FileBasicInfo.FileAttributes =
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_VIRTUAL;

        // Store content id
        placeholderInfo.VersionInfo.ContentID[0] = static_cast<UINT8>(options & 0xFF);
        placeholderInfo.VersionInfo.ContentID[1] = static_cast<UINT8>((options >> 8)  & 0xFF);
        placeholderInfo.VersionInfo.ContentID[2] = static_cast<UINT8>((options >> 16) & 0xFF);
        placeholderInfo.VersionInfo.ContentID[3] = static_cast<UINT8>((options >> 24) & 0xFF);
        placeholderInfo.VersionInfo.ContentID[4] = static_cast<UINT8>(draftScale);

        // Use current time
        FILETIME currentTime;
        LARGE_INTEGER currentTimeLargeInteger;

        GetSystemTimeAsFileTime(&currentTime);

        currentTimeLargeInteger.LowPart = currentTime.dwLowDateTime;
        currentTimeLargeInteger.HighPart = currentTime.dwHighDateTime;

        placeholderInfo.FileBasicInfo.CreationTime = currentTimeLargeInteger;
        placeholderInfo.FileBasicInfo.LastAccessTime = currentTimeLargeInteger;
        placeholderInfo.FileBasicInfo.LastWriteTime = currentTimeLargeInteger;
        placeholderInfo.FileBasicInfo.ChangeTime = currentTimeLargeInteger;
    }

class Session : public VirtualizationInstance {
public:
    Session(const std::string& dstPath, std::unique_ptr<VirtualFileSystemImpl_MCRAW> fs);
    ~Session();

public:
    void updateOptions(FileRenderOptions options, int draftScale);

protected:
    HRESULT StartDirEnum(_In_ const PRJ_CALLBACK_DATA* CallbackData, _In_ const GUID* EnumerationId) override;

    HRESULT EndDirEnum(_In_ const PRJ_CALLBACK_DATA* CallbackData, _In_ const GUID* EnumerationId) override;

    HRESULT GetDirEnum(
        _In_ const PRJ_CALLBACK_DATA* CallbackData,
        _In_ const GUID* EnumerationId,
        _In_opt_ PCWSTR SearchExpression,
        _In_ PRJ_DIR_ENTRY_BUFFER_HANDLE DirEntryBufferHandle) override;

    HRESULT GetPlaceholderInfo(_In_ const PRJ_CALLBACK_DATA* CallbackData) override;

    HRESULT GetFileData(_In_ const PRJ_CALLBACK_DATA* CallbackData, _In_ UINT64 ByteOffset, _In_ UINT32 Length) override;

    HRESULT Notify(
        _In_ const PRJ_CALLBACK_DATA* CallbackData,
        _In_ BOOLEAN IsDirectory,
        _In_ PRJ_NOTIFICATION NotificationType,
        _In_opt_ PCWSTR DestinationFileName,
        _Inout_ PRJ_NOTIFICATION_PARAMETERS* NotificationParameters) override;

private:
    FileRenderOptions mOptions;
    int mDraftScale;
    std::mutex mOpLock;
    std::unique_ptr<VirtualFileSystemImpl_MCRAW> mFs;
    std::map<GUID, std::unique_ptr<DirInfo>, GUIDComparer> mActiveEnumSessions;
};

Session::Session(
    const std::string& dstPath,
    std::unique_ptr<VirtualFileSystemImpl_MCRAW> fs) : mFs(std::move(fs))
{
    SetOptionalMethods(OptionalMethods::Notify);

    // Specify the notifications that we want ProjFS to send to us.  Everywhere under the virtualization
    // root we want ProjFS to tell us when files have been opened, when they're about to be renamed,
    // and when they're about to be deleted.
    PRJ_NOTIFICATION_MAPPING notificationMappings[] = {
        {
            PRJ_NOTIFY_FILE_OPENED                      |
            PRJ_NOTIFY_NEW_FILE_CREATED                 |
            PRJ_NOTIFY_FILE_OVERWRITTEN                 |
            PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED |
            PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED  |
            PRJ_NOTIFY_FILE_RENAMED                     |
            PRJ_NOTIFY_HARDLINK_CREATED                 |
            PRJ_NOTIFY_PRE_DELETE                       |
            PRJ_NOTIFY_PRE_RENAME                       |
            PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL         |
            PRJ_NOTIFY_PRE_SET_HARDLINK,
            L""
        }
    };

    // Store the notification mapping we set up into a start options structure.  We leave all the
    // other options at their defaults.
    PRJ_STARTVIRTUALIZING_OPTIONS prjOptions = {};

    prjOptions.NotificationMappings = notificationMappings;
    prjOptions.NotificationMappingsCount = 1;

    auto hr = this->Start(fromUTF8(dstPath).c_str(), &prjOptions);
    if(hr != S_OK) {
        throw std::runtime_error("Failed to create mount point (error: + " + std::to_string(hr) + ")");
    }
}

Session::~Session() {
    Stop();
}

void Session::updateOptions(FileRenderOptions options, int draftScale) {
    mOptions = options;
    mDraftScale = draftScale;

    // Tell file system about new options
    mFs->updateOptions(options, draftScale);

    // We need to clear out the cache
    auto files = mFs->listFiles();
    HRESULT hr = S_OK;

    PRJ_UPDATE_FAILURE_CAUSES failureReason;

    // Use these flags to invalidate the cache without deleting
    PRJ_UPDATE_TYPES updateFlags =
        PRJ_UPDATE_ALLOW_DIRTY_METADATA |
        PRJ_UPDATE_ALLOW_DIRTY_DATA     |
        PRJ_UPDATE_ALLOW_READ_ONLY;

    for(auto& e : files) {
        if(e.type != EntryType::FILE_ENTRY)
            continue;

        auto fullPath = e.getFullPath().string();

        // hr = PrjDeleteFile(_instanceHandle, fromUTF8(fullPath).c_str(), updateFlags, &failureReason);

        // Only DNG items need to be updated with options changes
        if(boost::ends_with(e.name, "dng")) {
            PRJ_PLACEHOLDER_INFO placeholderInfo = {};

            updatePlaceHolder(placeholderInfo, e, options, draftScale);

            hr = PrjUpdateFileIfNeeded(
                _instanceHandle,
                fromUTF8(fullPath).c_str(),
                &placeholderInfo,
                sizeof(placeholderInfo),
                PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_DIRTY_DATA | PRJ_UPDATE_ALLOW_READ_ONLY,
                &failureReason
            );

            // Ignore file not found errors
            if(failureReason != PRJ_UPDATE_FAILURE_CAUSE_NONE)
                spdlog::error("Failed to refresh cache entry {} (error: 0x{:08x}, reason: {})",
                              fullPath, static_cast<unsigned int>(hr), static_cast<unsigned int>(failureReason));
        }
    }
}

HRESULT Session::StartDirEnum(_In_ const PRJ_CALLBACK_DATA* CallbackData, _In_ const GUID* EnumerationId) {
    spdlog::debug("StartDirEnum(): Path [{}] triggered by [{}]",
        toUTF8(CallbackData->FilePathName),
        toUTF8(CallbackData->TriggeringProcessImageFileName));

    std::lock_guard<std::mutex> guard(mOpLock);

    mActiveEnumSessions[*EnumerationId] = std::make_unique<DirInfo>(CallbackData->FilePathName);

    return S_OK;
}

HRESULT Session::EndDirEnum(_In_ const PRJ_CALLBACK_DATA* CallbackData, _In_ const GUID* EnumerationId) {
    spdlog::debug("EndDirEnum()");

    std::lock_guard<std::mutex> guard(mOpLock);

    // Get rid of the DirInfo object we created in StartDirEnum.
    mActiveEnumSessions.erase(*EnumerationId);

    return S_OK;
}

HRESULT Session::GetDirEnum(
    _In_ const PRJ_CALLBACK_DATA* CallbackData,
    _In_ const GUID* EnumerationId,
    _In_opt_ PCWSTR SearchExpression,
    _In_ PRJ_DIR_ENTRY_BUFFER_HANDLE DirEntryBufferHandle)
{
    // Then your log statement becomes:
    spdlog::debug("GetDirEnum(): Path [{}] SearchExpression [{}]",
        toUTF8(CallbackData->FilePathName),
        toUTF8(SearchExpression));

    HRESULT hr = S_OK;

    std::lock_guard<std::mutex> guard(mOpLock);

    // Get the correct enumeration session from our map.
    auto it = mActiveEnumSessions.find(*EnumerationId);
    if (it == mActiveEnumSessions.end())
    {
        // We were asked for an enumeration we don't know about.
        hr = E_INVALIDARG;

        spdlog::debug("GetDirEnum(): return 0x{:08x}", static_cast<unsigned int>(hr));

        return hr;
    }

    // Get out our DirInfo helper object, which manages the context for this enumeration.
    auto& dirInfo = it->second;

    // If the enumeration is restarting, reset our bookkeeping information.
    if (CallbackData->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN)
    {
        dirInfo->Reset();
    }

    if (!dirInfo->EntriesFilled())
    {
        // Fill the directory info structure
        auto files = mFs->listFiles(toUTF8(SearchExpression));

        for(auto& x : files) {
            if(x.type == EntryType::DIRECTORY_ENTRY)
                dirInfo->FillDirEntry(fromUTF8(x.name).c_str());
            else if(x.type == EntryType::FILE_ENTRY)
                dirInfo->FillFileEntry(fromUTF8(x.name).c_str(), x.size);
        }

        // This will ensure the entries in the DirInfo are sorted the way the file system expects.
        dirInfo->SortEntriesAndMarkFilled();
    }

    // Return our directory entries to ProjFS.
    while (dirInfo->CurrentIsValid())
    {
        // ProjFS allocates a fixed size buffer then invokes this callback.  The callback needs to
        // call PrjFillDirEntryBuffer to fill as many entries as possible until the buffer is full.
        auto basicInfo = dirInfo->CurrentBasicInfo();

        if (PrjFillDirEntryBuffer(dirInfo->CurrentFileName(), &basicInfo, DirEntryBufferHandle) != S_OK)
            break;

        // Only move the current entry cursor after the entry was successfully filled, so that we
        // can start from the correct index in the next GetDirEnum callback for this enumeration
        // session.
        dirInfo->MoveNext();
    }

    return hr;
}

HRESULT Session::GetPlaceholderInfo(_In_ const PRJ_CALLBACK_DATA* CallbackData) {
    const auto filename = toUTF8(CallbackData->FilePathName);

    spdlog::debug("GetPlaceholderInfo(): Path [{}] triggered by [{}]",
        filename,
        toUTF8(CallbackData->TriggeringProcessImageFileName));

    bool isKey;
    INT64 valSize = 0;

    auto optionalEntry = mFs->findEntry(filename);
    if(!optionalEntry.has_value()) {
        spdlog::error("GetPlaceholderInfo(file: {}): return 0x{:08x}",
            filename, static_cast<unsigned int>(ERROR_FILE_NOT_FOUND));

        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    auto entry = optionalEntry.value();

    PRJ_PLACEHOLDER_INFO placeholderInfo = {};

    updatePlaceHolder(placeholderInfo, entry, mOptions, mDraftScale);

    // Create the on-disk placeholder.
    HRESULT hr = WritePlaceholderInfo(
        CallbackData->FilePathName, &placeholderInfo, sizeof(placeholderInfo));

    if(FAILED(hr))
        spdlog::error("GetPlaceholderInfo(): return 0x{:08x}", static_cast<unsigned int>(hr));

    return hr;
}

HRESULT Session::GetFileData(_In_ const PRJ_CALLBACK_DATA* callbackData, _In_ UINT64 byteOffset, _In_ UINT32 length) {
    spdlog::debug("GetFileData(): Path [{}] (byteOffset: {} and length: {}) triggered by [{}]",
                  toUTF8(callbackData->FilePathName),
                  byteOffset,
                  length,
                  toUTF8(callbackData->TriggeringProcessImageFileName));

    HRESULT hr = S_OK;

    // Match file entry first
    auto fsEntry = mFs->findEntry(toUTF8(callbackData->FilePathName));
    if(!fsEntry) {
        hr = E_FAIL;
        return hr;
    }

    // We're going to need alignment information that is stored in the instance to service this callback.
    PRJ_VIRTUALIZATION_INSTANCE_INFO instanceInfo;
    hr = PrjGetVirtualizationInstanceInfo(_instanceHandle, &instanceInfo);

    if (FAILED(hr))
    {
        spdlog::error("GetFileData(): PrjGetVirtualizationInstanceInfo error: 0x{:08x}", static_cast<unsigned int>(hr));

        return hr;
    }

    auto commandId = callbackData->CommandId;
    auto dataStramId = callbackData->DataStreamId;
    auto fileName = toUTF8(callbackData->FilePathName);

    // Allocate a buffer that adheres to the machine's memory alignment.  We have to do this in case
    // the caller who caused this callback to be invoked is performing non-cached I/O.  For more
    // details, see the topic "Providing File Data" in the ProjFS documentation.
    void* writeBuffer = PrjAllocateAlignedBuffer(_instanceHandle, length);
    if (writeBuffer == nullptr)
    {
        spdlog::error("GetFileData(): Could not allocate write buffer");

        return E_OUTOFMEMORY;
    }

    auto completeTransaction = [this, writeBuffer, byteOffset, length, fileName, commandId, dataStramId](size_t readBytes, int error, bool isAsync) {
        HRESULT hr = S_OK;

        if(readBytes == length) {
            hr = WriteFileData(&dataStramId, reinterpret_cast<PVOID>(writeBuffer), byteOffset, length);
        }
        else {
            hr = E_FAIL;
            spdlog::error("GetFileData(): Failed to read file requested bytes {} but received {}", length, readBytes);
        }

        if (FAILED(hr))
        {
            // If this callback returns an error, ProjFS will return this error code to the thread that
            // issued the file read, and the target file will remain an empty placeholder.
            spdlog::error("GetFileData(): failed to write file for [%s]: 0x{:08x}", fileName, static_cast<unsigned int>(hr));
        }

        // Free the memory-aligned buffer we allocated.
        PrjFreeAlignedBuffer(writeBuffer);

        if(FAILED(hr))
            spdlog::error("GetFileData(): Return 0x{:08x}", static_cast<unsigned int>(hr));

        if(isAsync)
            PrjCompleteCommand(_instanceHandle, commandId, hr, nullptr);
    };

    auto asyncCompleteTransaction = std::bind(completeTransaction, std::placeholders::_1, std::placeholders::_2, true);

    // Read the data asynchronously
    auto result = mFs->readFile(
        *fsEntry,
        byteOffset,
        length,
        writeBuffer,
        asyncCompleteTransaction,
        true);

    if(result > 0) {
        completeTransaction(result, 0, false);
        return hr;
    }
    else // async read
        return HRESULT_FROM_WIN32(ERROR_IO_PENDING);
}

HRESULT Session::Notify(
    _In_ const PRJ_CALLBACK_DATA* CallbackData,
    _In_ BOOLEAN IsDirectory,
    _In_ PRJ_NOTIFICATION NotificationType,
    _In_opt_ PCWSTR DestinationFileName,
    _Inout_ PRJ_NOTIFICATION_PARAMETERS* NotificationParameters) {

    spdlog::debug("{}: Path [{}] triggered by [{}] Notification: 0x{:08x}",
                 __FUNCTION__,
                 toUTF8(CallbackData->FilePathName),
                 toUTF8(CallbackData->TriggeringProcessImageFileName),
                static_cast<unsigned int>(NotificationType));

    switch (NotificationType)
    {
    case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
    case PRJ_NOTIFICATION_FILE_OPENED:
        return S_OK;

    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED:
    case PRJ_NOTIFICATION_FILE_OVERWRITTEN:
    case PRJ_NOTIFICATION_NEW_FILE_CREATED:
    case PRJ_NOTIFICATION_FILE_RENAMED:
    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED:
    case PRJ_NOTIFICATION_PRE_RENAME:
    case PRJ_NOTIFICATION_PRE_DELETE:
        // Deny all write operations
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);

    default:
        return S_OK;
    }
}

void setupLogging() {
    try {
        // Create a vector of sinks
        std::vector<spdlog::sink_ptr> sinks;

        // Regular console output
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // File sink
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/logfile.txt", true));

        // For Windows/Visual Studio debugger
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());

        // Create a logger with all sinks
        auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());

        // Set as default logger
        spdlog::set_default_logger(logger);

        // Set log level
#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif

        // Flush on info level messages
        spdlog::flush_on(spdlog::level::info);

    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
}

} // namespace

FuseFileSystemImpl_Win::FuseFileSystemImpl_Win() :
    mNextMountId(0),
    mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
    mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
    mCache(std::make_unique<LRUCache>(CACHE_SIZE))
{
    setupLogging();
}

MountId FuseFileSystemImpl_Win::mount(FileRenderOptions options, int draftScale, const std::string& srcFile, const std::string& dstPath) {
    fs::path srcPath(srcFile);
    std::string extension = srcPath.extension().string();

    spdlog::debug("Mounting file {} to {}", srcFile, dstPath);

    if(boost::iequals(extension, ".mcraw")) {
        auto mountId = mNextMountId++;

        try {
            auto fs = std::make_unique<VirtualFileSystemImpl_MCRAW>(*mIoThreadPool, *mProcessingThreadPool, *mCache, options, draftScale, srcFile);

            mMountedFiles[mountId] = std::make_unique<Session>(dstPath, std::move(fs));
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to mount {} to {} (error: {})", srcFile, dstPath, e.what());

            throw std::runtime_error(e.what());
        }

        return mountId;
    }

    spdlog::error("Failed to mount {} to {}, invalid file format", srcFile, dstPath);

    throw std::runtime_error("Invalid format");
}

void FuseFileSystemImpl_Win::unmount(MountId mountId) {
    mMountedFiles.erase(mountId);
}

void FuseFileSystemImpl_Win::updateOptions(MountId mountId, FileRenderOptions options, int draftScale) {
    auto it = mMountedFiles.find(mountId);
    if(it == mMountedFiles.end())
        return;

    dynamic_cast<Session*>(mMountedFiles[mountId].get())->updateOptions(
        options, draftScale);
}

} // namespace motioncam

#include "win/FuseFileSystemImpl_Win.h"
#include "win/dirInfo.h"
#include "win/virtualizationInstance.h"

#include "IVirtualFileSystem.h"
#include "VirtualFileSystemImpl_MCRAW.h"
#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl_DNG.h"
#include "DNGDecoder.h"
#include "LRUCache.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "Utils.h"
#include <motioncam/Decoder.hpp>

#include <iostream>
#include <ntstatus.h>
#include <mutex>
#include <filesystem>
#include <shlobj.h>
#include <atomic>
#include <thread>
#include <chrono>

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

    void updatePlaceHolder(PRJ_PLACEHOLDER_INFO& placeholderInfo, const Entry& entry,
                           const RenderSettings& config, uint64_t contentVersion) {
        placeholderInfo.FileBasicInfo.IsDirectory = entry.type == EntryType::DIRECTORY_ENTRY;
        placeholderInfo.FileBasicInfo.FileSize = entry.size;
        placeholderInfo.FileBasicInfo.FileAttributes =
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_VIRTUAL;

        // Store content id
        placeholderInfo.VersionInfo.ContentID[0] = static_cast<UINT8>(config.options & 0xFF);
        placeholderInfo.VersionInfo.ContentID[1] = static_cast<UINT8>((config.options >> 8)  & 0xFF);
        placeholderInfo.VersionInfo.ContentID[2] = static_cast<UINT8>((config.options >> 16) & 0xFF);
        placeholderInfo.VersionInfo.ContentID[3] = static_cast<UINT8>((config.options >> 24) & 0xFF);
        placeholderInfo.VersionInfo.ContentID[4] = static_cast<UINT8>(config.draftScale);
        for (size_t i = 0; i < sizeof(contentVersion); ++i) {
            placeholderInfo.VersionInfo.ContentID[8 + i] =
                static_cast<UINT8>((contentVersion >> (i * 8)) & 0xff);
        }

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
    Session(const std::string& srcPath, const std::string& dstPath,
            std::unique_ptr<IVirtualFileSystem> fs);
    ~Session();

public:
    void updateOptions(const RenderSettings& settings);
    FileInfo getFileInfo() const;
    const std::string& sourcePath() const { return mSrcPath; }
    bool generateThumbnail(const std::string& path, int width, int height) {
        return mFs->generateThumbnail(path, width, height);
    }
    const std::string& destinationPath() const { return mDstPath; }
    HRESULT dehydrate(const std::filesystem::path& relativePath) {
        PRJ_UPDATE_FAILURE_CAUSES cause = PRJ_UPDATE_FAILURE_CAUSE_NONE;
        return PrjDeleteFile(_instanceHandle, relativePath.wstring().c_str(),
            PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_DIRTY_DATA |
            PRJ_UPDATE_ALLOW_READ_ONLY, &cause);
    }
    void finalize(const std::string&, bool, const FinalizeOptions&,
        const std::function<bool(size_t, size_t, const std::string&)>&,
        const std::function<void(const std::vector<uint8_t>&, Timestamp)>&, bool);

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
    std::string mSrcPath;
    std::string mDstPath;
    RenderSettings mConfig;
    std::mutex mOpLock;
    std::atomic_uint64_t mContentVersion{0};
    std::unique_ptr<IVirtualFileSystem> mFs;
    std::map<GUID, std::unique_ptr<DirInfo>, GUIDComparer> mActiveEnumSessions;
};

Session::Session(
    const std::string& srcPath,
    const std::string& dstPath,
    std::unique_ptr<IVirtualFileSystem> fs)
    : mSrcPath(srcPath), mDstPath(dstPath), mFs(std::move(fs))
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

void Session::updateOptions(const RenderSettings& settings) {
    mConfig = settings;
    mFs->updateOptions(settings);
    ++mContentVersion;

    // We need to clear out the cache
    auto files = mFs->listFiles("");
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

            updatePlaceHolder(placeholderInfo, e, mConfig, mContentVersion.load());

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

FileInfo Session::getFileInfo() const {
    return mFs->getFileInfo();
}

void Session::finalize(
    const std::string& destination, bool jpegCompression, const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    vfs::finalize(*mFs, destination, jpegCompression, options, progress, fileReady, writeFiles);
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
        auto files = mFs->listFiles("");

        for(auto& x : files) {
            const auto wideName = fromUTF8(x.name);
            if (!PrjFileNameMatch(wideName.c_str(), SearchExpression)) {
                continue;
            }
            if(x.type == EntryType::DIRECTORY_ENTRY)
                dirInfo->FillDirEntry(wideName.c_str());
            else if(x.type == EntryType::FILE_ENTRY)
                dirInfo->FillFileEntry(wideName.c_str(), x.size);
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

    updatePlaceHolder(placeholderInfo, entry, mConfig, mContentVersion.load());

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

    auto completeTransaction = [this, writeBuffer, byteOffset, length, fileName, commandId, dataStramId, fsEntry](size_t readBytes, int error, bool isAsync) {
        HRESULT hr = S_OK;

        if(readBytes > 0 && readBytes <= length) {
            // Write the actual bytes read (may be less than requested for compressed files)
            hr = WriteFileData(&dataStramId, reinterpret_cast<PVOID>(writeBuffer), byteOffset, static_cast<DWORD>(readBytes));
            
            if(readBytes < length) {
                spdlog::debug("GetFileData(): Wrote {} bytes (requested {}), file may be compressed", readBytes, length);
            }
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

std::string getLogDirectory() {
    std::string logDir;
    wchar_t* appDataPath = nullptr;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appDataPath))) {
        // Convert wide string to string
        int size = WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, nullptr, 0, nullptr, nullptr);
        std::string appData(size - 1, '\0');

        WideCharToMultiByte(CP_UTF8, 0, appDataPath, -1, &appData[0], size, nullptr, nullptr);
        CoTaskMemFree(appDataPath);

        logDir = appData + "\\MotionCam Tools\\Fuse\\logs";
    }
    else {
        // Fallback to temp directory
        logDir = std::filesystem::temp_directory_path().string() + "\\MotionCam Tools\\Fuse\\logs";
    }

    return logDir;
}

void setupLogging() {
    try {
        // Get platform-appropriate log directory
        std::string logDir = getLogDirectory();

        // Create the log directory if it doesn't exist
        std::filesystem::create_directories(logDir);

        // Create log file path
        std::string logFilePath = logDir + "/logfile.txt";

        // Create a vector of sinks
        std::vector<spdlog::sink_ptr> sinks;

        // Regular console output
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // File sink with the proper path
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, true));

#ifdef _WIN32
        // For Windows/Visual Studio debugger
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif

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

        // Log the successful initialization and file location
        spdlog::info("Logging initialized. Log file: {}", logFilePath);
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
    catch (const std::filesystem::filesystem_error& ex) {
        std::cerr << "Failed to create log directory: " << ex.what() << std::endl;
    }
}

} // namespace motioncam

FuseFileSystemImpl_Win::FuseFileSystemImpl_Win() :
    mNextMountId(0),
    mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
    mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
    mCache(std::make_unique<LRUCache>(CACHE_SIZE))
{
    setupLogging();
}

FuseFileSystemImpl_Win::~FuseFileSystemImpl_Win() = default;

MountId FuseFileSystemImpl_Win::mount(const RenderSettings& settings, const std::string& srcFile, const std::string& dstPath) {
    fs::path srcPath(srcFile);
    std::string extension = srcPath.extension().string();
    std::string filename = srcPath.filename().string();

    auto normalizedSource = fs::absolute(srcPath).lexically_normal().wstring();
    auto normalizedDestination = fs::absolute(fs::path(dstPath)).lexically_normal().wstring();
    std::transform(normalizedSource.begin(), normalizedSource.end(), normalizedSource.begin(), ::towlower);
    std::transform(normalizedDestination.begin(), normalizedDestination.end(), normalizedDestination.begin(), ::towlower);
    if (normalizedSource == normalizedDestination)
        throw std::runtime_error("Source and mount destination must be different paths");

    spdlog::debug("Mounting file {} to {}", srcFile, dstPath);

    if(boost::iequals(extension, ".mcraw")) {
        auto mountId = mNextMountId++;

        try {
            // Extract base name from destination path
            fs::path dstPathObj(dstPath);
            std::string baseName = dstPathObj.filename().string();
            auto fs = std::make_unique<VirtualFileSystemImpl_MCRAW>(*mIoThreadPool, *mProcessingThreadPool, *mCache, settings, srcFile, baseName);
            auto session = std::make_shared<Session>(srcFile, dstPath, std::move(fs));
            std::lock_guard<std::mutex> lock(mMountedFilesMutex);
            mMountedFiles[mountId] = std::move(session);
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to mount {} to {} (error: {})", srcFile, dstPath, e.what());
            throw std::runtime_error(e.what());
        }
        return mountId;
    }
    else if((boost::iequals(extension, ".mov") || boost::iequals(extension, ".mp4") ||
             boost::iequals(extension, ".mkv")) &&
            boost::icontains(filename, "NATIVE")) {
        auto mountId = mNextMountId++;

        try {
            // Extract base name from destination path
            fs::path dstPathObj(dstPath);
            std::string baseName = dstPathObj.filename().string();
            auto fs = std::make_unique<VirtualFileSystemImpl_DirectLog>(*mIoThreadPool, *mProcessingThreadPool, *mCache, settings, srcFile, baseName);
            auto session = std::make_shared<Session>(srcFile, dstPath, std::move(fs));
            std::lock_guard<std::mutex> lock(mMountedFilesMutex);
            mMountedFiles[mountId] = std::move(session);
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to mount {} to {} (error: {})", srcFile, dstPath, e.what());
            throw std::runtime_error(e.what());
        }
        return mountId;
    }
    else if(boost::iequals(extension, ".dng") || DNGDecoder::isDNGSequence(srcFile)) {
        auto mountId = mNextMountId++;

        try {
            // Extract base name from destination path
            fs::path dstPathObj(dstPath);
            std::string baseName = dstPathObj.filename().string();
            auto fs = std::make_unique<VirtualFileSystemImpl_DNG>(*mIoThreadPool, *mProcessingThreadPool, *mCache, settings, srcFile, baseName);
            auto session = std::make_shared<Session>(srcFile, dstPath, std::move(fs));
            std::lock_guard<std::mutex> lock(mMountedFilesMutex);
            mMountedFiles[mountId] = std::move(session);
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
    std::shared_ptr<VirtualizationInstance> session;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) return;
        session = std::move(it->second);
        mMountedFiles.erase(it);
    }
    session.reset();
}

void FuseFileSystemImpl_Win::updateOptions(MountId mountId, const RenderSettings& settings) {
    std::shared_ptr<VirtualizationInstance> instance;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it != mMountedFiles.end()) instance = it->second;
    }
    if (auto* session = dynamic_cast<Session*>(instance.get())) session->updateOptions(settings);
}

std::optional<FileInfo> FuseFileSystemImpl_Win::getFileInfo(MountId mountId) {
    std::shared_ptr<VirtualizationInstance> instance;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it != mMountedFiles.end()) instance = it->second;
    }
    if (auto* session = dynamic_cast<Session*>(instance.get())) return session->getFileInfo();
    return std::nullopt;
}

bool FuseFileSystemImpl_Win::generateThumbnail(
    MountId mountId, const std::string& outputPath, int width, int height) {
    std::shared_ptr<VirtualizationInstance> instance;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) return false;
        instance = it->second;
    }
    auto* session = dynamic_cast<Session*>(instance.get());
    if (!session) return false;
    const std::string sourcePath = session->sourcePath();
    if (!boost::iequals(fs::path(sourcePath).extension().string(), ".mcraw"))
        return session->generateThumbnail(outputPath, width, height);
    try {
        const fs::path source(sourcePath);
        Decoder decoder(source.string());
        auto frames = decoder.getFrames();
        if (frames.empty()) return false;
        std::sort(frames.begin(), frames.end());
        std::vector<uint8_t> data;
        nlohmann::json metadata;
        decoder.loadFrame(frames.front(), data, metadata);
        return utils::generateJpegThumbnail(
            data, CameraFrameMetadata::parse(metadata),
            CameraConfiguration::parse(decoder.getContainerMetadata()),
            outputPath, width, height);
    } catch (const std::exception& error) {
        spdlog::warn("Could not generate thumbnail: {}", error.what());
        return false;
    }
}

void FuseFileSystemImpl_Win::setCachePolicy(CachePolicy policy) {
    mCachePolicy = policy;
}

void FuseFileSystemImpl_Win::setCacheQuotaBytes(std::uint64_t bytes) {
    mCacheQuotaBytes = bytes;
}

void FuseFileSystemImpl_Win::cleanupCacheExpired() {
    if (mCachePolicy != CachePolicy::Quota || mCacheQuotaBytes == 0) return;
    std::lock_guard<std::mutex> mountedFilesLock(mMountedFilesMutex);
    struct Candidate {
        Session* session;
        std::filesystem::path relativePath;
        std::uint64_t allocatedBytes;
        std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    std::uint64_t totalBytes = 0;
    for (auto& mounted : mMountedFiles) {
        auto* session = dynamic_cast<Session*>(mounted.second.get());
        if (!session) continue;
        const std::filesystem::path root(session->destinationPath());
        std::error_code error;
        if (!std::filesystem::exists(root, error)) continue;
        for (std::filesystem::recursive_directory_iterator current(
                 root, std::filesystem::directory_options::skip_permission_denied, error), end;
             current != end; current.increment(error)) {
            if (error) { error.clear(); continue; }
            if (!current->is_regular_file(error) ||
                !boost::iequals(current->path().extension().string(), ".dng"))
                continue;
            DWORD high = 0;
            SetLastError(NO_ERROR);
            const DWORD low = GetCompressedFileSizeW(current->path().c_str(), &high);
            if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) continue;
            const std::uint64_t bytes = (static_cast<std::uint64_t>(high) << 32) | low;
            if (bytes == 0) continue;
            totalBytes += bytes;
            candidates.push_back({session, std::filesystem::relative(current->path(), root, error),
                                  bytes, current->last_write_time(error)});
        }
    }
    if (totalBytes <= mCacheQuotaBytes) return;
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.modified < right.modified;
    });
    for (const auto& candidate : candidates) {
        if (totalBytes <= mCacheQuotaBytes) break;
        const HRESULT result = candidate.session->dehydrate(candidate.relativePath);
        if (SUCCEEDED(result)) totalBytes -= (std::min)(totalBytes, candidate.allocatedBytes);
        else spdlog::debug("Could not evict projected DNG {} (0x{:08x})",
                           candidate.relativePath.string(), static_cast<unsigned int>(result));
    }
}

void FuseFileSystemImpl_Win::finalize(
    MountId mountId, const std::string& destination, bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    std::shared_ptr<VirtualizationInstance> instance;
    {
        std::lock_guard<std::mutex> lock(mMountedFilesMutex);
        const auto it = mMountedFiles.find(mountId);
        if (it == mMountedFiles.end()) throw std::runtime_error("Mount not found");
        instance = it->second;
    }
    auto* session = dynamic_cast<Session*>(instance.get());
    if (!session) throw std::runtime_error("Invalid mount session");
    session->finalize(
        destination, jpegCompression, options, progress, fileReady, writeFiles);
}

}

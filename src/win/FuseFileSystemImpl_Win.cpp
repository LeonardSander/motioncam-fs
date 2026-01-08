#include "win/FuseFileSystemImpl_Win.h"
#include "win/dirInfo.h"
#include "win/virtualizationInstance.h"

#include "VirtualFileSystemImpl_MCRAW.h"
#include "LRUCache.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "Utils.h"

#include <iostream>
#include <ntstatus.h>
#include <mutex>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <optional>
#include <shlobj.h>
#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/locale.hpp>

#include <BS_thread_pool.hpp>

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

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
    void updateOptions(
        FileRenderOptions options,
        int draftScale,
        std::string cfrTarget,
        std::string cropTarget,
        std::string cameraModel,
        std::string levels,
        std::string logTransform,
        std::string exposureCompensation,
        std::string quadBayerOption,
        bool matrixOverrideEnabled,
        std::string matrixProfile,
        std::string matrixFilePath);
    void expireMaterializedFiles(std::chrono::seconds ttl);
    void evictMaterializedByQuota(std::uint64_t quotaBytes);
    FileInfo getFileInfo() const;
    VirtualFileSystemImpl_MCRAW* getFileSystem() { return mFs.get(); }
    const std::wstring& getRootPath() const { return _rootPath; }
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT getInstanceHandle() const { return _instanceHandle; }

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

void Session::updateOptions(
    FileRenderOptions options,
    int draftScale,
    std::string cfrTarget,
    std::string cropTarget,
    std::string cameraModel,
    std::string levels,
    std::string logTransform,
    std::string exposureCompensation,
    std::string quadBayerOption,
    bool matrixOverrideEnabled,
    std::string matrixProfile,
    std::string matrixFilePath) {
    mOptions = options;
    mDraftScale = draftScale;
    mFs->updateOptions(
        options,
        draftScale,
        cfrTarget,
        cropTarget,
        cameraModel,
        levels,
        logTransform,
        exposureCompensation,
        quadBayerOption,
        false,
        "",
        "");

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

        // Only DNG items need to be updated with options changes
        if(boost::ends_with(e.name, "dng")) {
            // Delete the file placeholder to force complete refresh
            // This ensures all settings changes (not just proxy mode) trigger a full regeneration
            hr = PrjDeleteFile(_instanceHandle, fromUTF8(fullPath).c_str(), updateFlags, &failureReason);

            // Ignore errors - file might not be materialized yet or already deleted
            if(hr != S_OK && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                spdlog::debug("PrjDeleteFile for {} returned: 0x{:08x}, reason: {}",
                              fullPath, static_cast<unsigned int>(hr), static_cast<unsigned int>(failureReason));
            }
        }
    }
}

void Session::expireMaterializedFiles(std::chrono::seconds ttl) {
    if (ttl.count() <= 0) {
        return;
    }

    const auto expired = mFs->getExpiredDngEntries(ttl);
    if (expired.empty()) {
        return;
    }

    PRJ_UPDATE_FAILURE_CAUSES failureReason;
    PRJ_UPDATE_TYPES updateFlags =
        PRJ_UPDATE_ALLOW_DIRTY_METADATA |
        PRJ_UPDATE_ALLOW_DIRTY_DATA     |
        PRJ_UPDATE_ALLOW_READ_ONLY;

    for (const auto& entry : expired) {
        if (entry.type != EntryType::FILE_ENTRY) {
            continue;
        }
        if (!boost::ends_with(entry.name, "dng")) {
            continue;
        }

        auto fullPath = entry.getFullPath().string();
        auto hr = PrjDeleteFile(_instanceHandle, fromUTF8(fullPath).c_str(), updateFlags, &failureReason);
        if (hr != S_OK && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            spdlog::debug("PrjDeleteFile for {} returned: 0x{:08x}, reason: {}",
                          fullPath, static_cast<unsigned int>(hr), static_cast<unsigned int>(failureReason));
        }
    }
}

void Session::evictMaterializedByQuota(std::uint64_t quotaBytes) {
    if (quotaBytes == 0) {
        return;
    }

    auto entries = mFs->getDngAccessEntries();
    if (entries.empty()) {
        return;
    }

    std::uint64_t totalBytes = 0;
    for (const auto& entry : entries) {
        totalBytes += static_cast<std::uint64_t>(entry.first.size);
    }
    if (totalBytes <= quotaBytes) {
        return;
    }

    std::sort(entries.begin(), entries.end(),
        [](const auto& left, const auto& right) {
            return left.second < right.second;
        });

    PRJ_UPDATE_FAILURE_CAUSES failureReason;
    PRJ_UPDATE_TYPES updateFlags =
        PRJ_UPDATE_ALLOW_DIRTY_METADATA |
        PRJ_UPDATE_ALLOW_DIRTY_DATA     |
        PRJ_UPDATE_ALLOW_READ_ONLY;

    for (const auto& entryPair : entries) {
        if (totalBytes <= quotaBytes) {
            break;
        }

        const auto& entry = entryPair.first;
        if (entry.type != EntryType::FILE_ENTRY) {
            continue;
        }
        auto fullPath = entry.getFullPath().string();
        auto hr = PrjDeleteFile(_instanceHandle, fromUTF8(fullPath).c_str(), updateFlags, &failureReason);
        if (hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            if (entry.size < totalBytes) {
                totalBytes -= static_cast<std::uint64_t>(entry.size);
            } else {
                totalBytes = 0;
            }
            mFs->forgetDngAccess(entry);
        } else {
            spdlog::debug("PrjDeleteFile for {} returned: 0x{:08x}, reason: {}",
                          fullPath,
                          static_cast<unsigned int>(hr),
                          static_cast<unsigned int>(failureReason));
        }
    }
}

FileInfo Session::getFileInfo() const {
    return mFs->getFileInfo();
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
    auto requestStartTime = std::chrono::high_resolution_clock::now();
    auto t_entry = requestStartTime;
    auto fileName = toUTF8(callbackData->FilePathName);
    auto t_path = std::chrono::high_resolution_clock::now();
    auto d_path = std::chrono::duration_cast<std::chrono::milliseconds>(t_path - t_entry).count();

    spdlog::info("[TIMING] GetFileData() entry: file={} d_path={}ms", fileName, d_path);

    spdlog::debug("GetFileData(): Path [{}] (byteOffset: {} and length: {}) triggered by [{}]",
                  fileName,
                  byteOffset,
                  length,
                  toUTF8(callbackData->TriggeringProcessImageFileName));        
    spdlog::info("[PERF] GetFileData() START: file={}, offset={}, length={} bytes",
                  fileName, byteOffset, length);

    // Track concurrent calls and lock wait timing
    static std::atomic<int> concurrentCalls{0};
    const int concurrent = concurrentCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    struct ConcurrentGuard {
        std::atomic<int>& ref;
        ~ConcurrentGuard() { ref.fetch_sub(1, std::memory_order_relaxed); }
    } concurrentGuard{concurrentCalls};

    // Fine-grained lock (previously coarse) - now removed to avoid contention.
    // We keep timing to validate that lock wait stays near zero.
    auto t_beforeLock = std::chrono::high_resolution_clock::now();
    auto d_beforeLock = std::chrono::duration_cast<std::chrono::milliseconds>(t_beforeLock - t_entry).count();

    spdlog::warn("[LOCK] Attempting lock: file={} concurrent_calls={} d_beforeLock={}ms",
                 fileName, concurrent, d_beforeLock);

    HRESULT hr = S_OK;
    std::optional<Entry> fsEntry;
    fsEntry = mFs->findEntry(toUTF8(callbackData->FilePathName));

    auto t_afterLock = std::chrono::high_resolution_clock::now();
    auto d_lockWait = std::chrono::duration_cast<std::chrono::milliseconds>(t_afterLock - t_beforeLock).count();
    auto d_total = std::chrono::duration_cast<std::chrono::milliseconds>(t_afterLock - t_entry).count();
    if (d_lockWait > 50) {
        spdlog::warn("[LOCK] SLOW lock acquisition: file={} wait={}ms total={}ms concurrent={}",
                     fileName, d_lockWait, d_total, concurrent);
    }

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

    auto t_beforeDispatch = std::chrono::high_resolution_clock::now();
    auto d_dispatchDelay = std::chrono::duration_cast<std::chrono::milliseconds>(t_beforeDispatch - t_entry).count();
    spdlog::warn("[DISPATCH] GetFileData pre-dispatch: file={} dispatchDelay={}ms", fileName, d_dispatchDelay);

    // Allocate a buffer that adheres to the machine's memory alignment.  We have to do this in case
    // the caller who caused this callback to be invoked is performing non-cached I/O.  For more
    // details, see the topic "Providing File Data" in the ProjFS documentation.
    void* writeBuffer = PrjAllocateAlignedBuffer(_instanceHandle, length);
    if (writeBuffer == nullptr)
    {
        spdlog::error("GetFileData(): Could not allocate write buffer");

        return E_OUTOFMEMORY;
    }

    auto completeTransaction = [this, writeBuffer, byteOffset, length, fileName, commandId, dataStramId, requestStartTime](size_t readBytes, int error, bool isAsync) {
        auto writeStartTime = std::chrono::high_resolution_clock::now();
        auto readMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeStartTime - requestStartTime).count();

        HRESULT hr = S_OK;

        if(readBytes == length) {
            hr = WriteFileData(&dataStramId, reinterpret_cast<PVOID>(writeBuffer), byteOffset, length);
        }
        else {
            hr = E_FAIL;
            spdlog::error("GetFileData(): Failed to read file requested bytes {} but received {}", length, readBytes);
        }

        auto writeEndTime = std::chrono::high_resolution_clock::now();
        auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeEndTime - writeStartTime).count();
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeEndTime - requestStartTime).count();

        if (FAILED(hr))
        {
            // If this callback returns an error, ProjFS will return this error code to the thread that
            // issued the file read, and the target file will remain an empty placeholder.
            spdlog::error("GetFileData(): failed to write file for [%s]: 0x{:08x}", fileName, static_cast<unsigned int>(hr));
        }
        else {
            spdlog::warn("[PERF] GetFileData() SUCCESS: file={}, read={}ms, write={}ms, TOTAL={}ms ({} bytes)",
                fileName, readMs, writeMs, totalMs, length);
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
    mCache(std::make_unique<LRUCache>(CACHE_SIZE)),
    mCachePolicy(CachePolicy::Quota),
    mCacheQuotaBytes(0)
{
    setupLogging();
}

void FuseFileSystemImpl_Win::setCachePolicy(CachePolicy policy) {
    mCachePolicy = policy;
}

void FuseFileSystemImpl_Win::setCacheQuotaBytes(std::uint64_t bytes) {
    mCacheQuotaBytes = bytes;
}

void FuseFileSystemImpl_Win::cleanupCacheExpired() {
    if (mCache) {
        mCache->cleanupExpired();
    }

    if (mCachePolicy == CachePolicy::Off) {
        return;
    }

    if (mCachePolicy == CachePolicy::Quota && mCacheQuotaBytes > 0) {
        evictMaterializedByGlobalQuota(mCacheQuotaBytes);
    }
}

void FuseFileSystemImpl_Win::evictMaterializedByGlobalQuota(std::uint64_t quotaBytes) {
    if (quotaBytes == 0) {
        return;
    }

    struct GlobalCandidate {
        Session* session;
        Entry entry;
        std::chrono::steady_clock::time_point lastAccess;
    };

    std::vector<GlobalCandidate> candidates;
    std::uint64_t totalBytes = 0;

    for (auto& entry : mMountedFiles) {
        auto* session = dynamic_cast<Session*>(entry.second.get());
        if (!session) {
            continue;
        }

        auto accessEntries = session->getFileSystem()->getDngAccessEntries();
        for (const auto& entryPair : accessEntries) {
            const auto& fileEntry = entryPair.first;
            totalBytes += static_cast<std::uint64_t>(fileEntry.size);
            candidates.push_back({session, fileEntry, entryPair.second});
        }
    }

    if (candidates.empty() || totalBytes <= quotaBytes) {
        return;
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const GlobalCandidate& left, const GlobalCandidate& right) {
            return left.lastAccess < right.lastAccess;
        });

    PRJ_UPDATE_FAILURE_CAUSES failureReason;
    PRJ_UPDATE_TYPES updateFlags =
        PRJ_UPDATE_ALLOW_DIRTY_METADATA |
        PRJ_UPDATE_ALLOW_DIRTY_DATA     |
        PRJ_UPDATE_ALLOW_READ_ONLY;

    for (const auto& candidate : candidates) {
        if (totalBytes <= quotaBytes) {
            break;
        }

        const auto& fileEntry = candidate.entry;
        if (fileEntry.type != EntryType::FILE_ENTRY) {
            continue;
        }

        auto relativePath = fileEntry.getFullPath().string();
        auto hr = PrjDeleteFile(candidate.session->getInstanceHandle(),
                                fromUTF8(relativePath).c_str(),
                                updateFlags,
                                &failureReason);
        if (hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            if (fileEntry.size < totalBytes) {
                totalBytes -= static_cast<std::uint64_t>(fileEntry.size);
            } else {
                totalBytes = 0;
            }

            candidate.session->getFileSystem()->forgetDngAccess(fileEntry);
        } else {
            spdlog::debug("PrjDeleteFile for {} returned: 0x{:08x}, reason: {}",
                          relativePath,
                          static_cast<unsigned int>(hr),
                          static_cast<unsigned int>(failureReason));
        }
    }
}

MountId FuseFileSystemImpl_Win::mount(
    FileRenderOptions options,
    int draftScale,
    std::string cfrTarget,
    std::string cropTarget,
    std::string cameraModel,
    std::string levels,
    std::string logTransform,
    std::string exposureCompensation,
    std::string quadBayerOption,
        bool matrixOverrideEnabled,
        const std::string& matrixProfile,
        const std::string& matrixFilePath,
        const std::string& srcFile,
        const std::string& dstPath) {
    fs::path srcPath(srcFile);
    std::string extension = srcPath.extension().string();

    spdlog::debug("Mounting file {} to {}", srcFile, dstPath);

    if(boost::iequals(extension, ".mcraw")) {
        auto mountId = mNextMountId++;

        try {
            fs::path dstPathObj(dstPath);
            // Extract base name from destination path
            std::string baseName = dstPathObj.filename().string();
            auto fs = std::make_unique<VirtualFileSystemImpl_MCRAW>(
                *mIoThreadPool,
                *mProcessingThreadPool,
                *mCache,
                options,
                draftScale,
                cfrTarget,
                cropTarget,
                srcFile,
                baseName,
                cameraModel,
                levels,
                logTransform,
                exposureCompensation,
                quadBayerOption,
                false,
                "",
                "");

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

void FuseFileSystemImpl_Win::updateOptions(
    MountId mountId,
    FileRenderOptions options,
    int draftScale,
    std::string cfrTarget,
    std::string cropTarget,
    std::string cameraModel,
    std::string levels,
    std::string logTransform,
    std::string exposureCompensation,
    std::string quadBayerOption,
    bool matrixOverrideEnabled,
    const std::string& matrixProfile,
    const std::string& matrixFilePath) {
    auto it = mMountedFiles.find(mountId);
    if(it == mMountedFiles.end())
        return;
    dynamic_cast<Session*>(mMountedFiles[mountId].get())->updateOptions(
        options,
        draftScale,
        cfrTarget,
        cropTarget,
        cameraModel,
        levels,
        logTransform,
        exposureCompensation,
        quadBayerOption,
        matrixOverrideEnabled,
        matrixProfile,
        matrixFilePath);
}

std::optional<FileInfo> FuseFileSystemImpl_Win::getFileInfo(MountId mountId) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        return dynamic_cast<Session*>(it->second.get())->getFileInfo();
    }
    return std::nullopt;
}

bool FuseFileSystemImpl_Win::generateThumbnail(MountId mountId, const std::string& outputPath, int width, int height) {
    auto it = mMountedFiles.find(mountId);
    if(it == mMountedFiles.end()) {
        spdlog::error("generateThumbnail(): Invalid mount ID {}", mountId);
        return false;
    }

    try {
        auto* session = dynamic_cast<Session*>(it->second.get());
        auto* fs = session->getFileSystem();
        const std::string& srcPath = fs->getSourcePath();

        // Create decoder and load frame 0
        Decoder decoder(srcPath);
        auto frames = decoder.getFrames();

        if(frames.empty()) {
            spdlog::error("generateThumbnail(): No frames in {}", srcPath);
            return false;
        }

        std::sort(frames.begin(), frames.end());
        auto timestamp = frames[0]; // Get first frame

        std::vector<uint8_t> data;
        nlohmann::json metadata;

        decoder.loadFrame(timestamp, data, metadata);

        auto frameMetadata = CameraFrameMetadata::parse(metadata);
        auto cameraConfiguration = CameraConfiguration::parse(decoder.getContainerMetadata());

        // Get current settings
        const std::string& levels = fs->getLevels();
        const std::string& exposureComp = fs->getExposureCompensation();

        return utils::generateJpegThumbnail(
            data,
            frameMetadata,
            cameraConfiguration,
            outputPath,
            width,
            height,
            levels,
            exposureComp
        );
    }
    catch(const std::exception& e) {
        spdlog::error("generateThumbnail(): Exception: {}", e.what());
        return false;
    }
}

}

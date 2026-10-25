#include "mainwindow.h"
#include "SingleApplication.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
#include "VulkanJpegFullEncode.h"
#include "VulkanJpegDecode.h"
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/gpu.h>
#endif
#include "liblj92/lj92.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>
#include <QFileInfo>
#include <QDirIterator>
#include <QDesktopServices>
#include <QFont>
#include <QGuiApplication>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include "CrashDebug.h"
#include <execinfo.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <exception>
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
std::vector<uint8_t> benchmarkCfaDng(const uint8_t* payload, size_t bytes,
                                     uint32_t width, uint32_t height,
                                     bool jpeg) {
    if (!payload || !width || !height || bytes > UINT32_MAX) return {};
    struct Entry { uint16_t tag, type; uint32_t count, value; };
    std::array<Entry, 17> entries{{
        {256, 4, 1, width}, {257, 4, 1, height},
        {258, 3, 1, 16}, {259, 3, 1, jpeg ? 7u : 1u},
        {262, 3, 1, 32803}, {273, 4, 1, 0},
        {277, 3, 1, 1}, {278, 4, 1, height},
        {279, 4, 1, static_cast<uint32_t>(bytes)},
        {284, 3, 1, 1}, {339, 3, 1, 1},
        {33421, 3, 2, 0x00020002},
        {33422, 1, 4, 0x02010100},
        {50706, 1, 4, 0x00000401},
        {50707, 1, 4, 0x00000301},
        {50714, 3, 1, 0}, {50717, 3, 1, 65535}
    }};
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    const uint32_t pixelOffset = 8u + 2u + uint32_t(entries.size()) * 12u + 4u;
    if (bytes > UINT32_MAX - pixelOffset) return {};
    entries[std::distance(entries.begin(), std::find_if(entries.begin(), entries.end(),
        [](const Entry& entry) { return entry.tag == 273; }))].value = pixelOffset;
    std::vector<uint8_t> dng(pixelOffset + bytes, 0);
    auto put16 = [&](size_t at, uint16_t value) {
        dng[at] = uint8_t(value); dng[at + 1] = uint8_t(value >> 8);
    };
    auto put32 = [&](size_t at, uint32_t value) {
        put16(at, uint16_t(value)); put16(at + 2, uint16_t(value >> 16));
    };
    dng[0] = 'I'; dng[1] = 'I'; put16(2, 42); put32(4, 8);
    put16(8, uint16_t(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t at = 10 + i * 12;
        put16(at, entries[i].tag); put16(at + 2, entries[i].type);
        put32(at + 4, entries[i].count); put32(at + 8, entries[i].value);
    }
    std::memcpy(dng.data() + pixelOffset, payload, bytes);
    return dng;
}

std::vector<uint8_t> benchmarkRgbDng(const uint8_t* payload, size_t bytes,
                                     uint32_t width, uint32_t height,
                                     bool jpeg) {
    if (!payload || !width || !height || bytes > UINT32_MAX) return {};
    struct Entry { uint16_t tag, type; uint32_t count, value; };
    std::array<Entry, 14> entries{{
        {256, 4, 1, width}, {257, 4, 1, height},
        {258, 3, 3, 0}, {259, 3, 1, jpeg ? 7u : 1u},
        {262, 3, 1, 34892}, {273, 4, 1, 0},
        {277, 3, 1, 3}, {278, 4, 1, height},
        {279, 4, 1, static_cast<uint32_t>(bytes)},
        {284, 3, 1, 1}, {50706, 1, 4, 0x00000401},
        {50707, 1, 4, 0x00000301},
        {50714, 3, 1, 0}, {50717, 3, 1, 65535}
    }};
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    const uint32_t bitsOffset = 8u + 2u + uint32_t(entries.size()) * 12u + 4u;
    const uint32_t pixelOffset = bitsOffset + 6u;
    if (bytes > UINT32_MAX - pixelOffset) return {};
    for (auto& entry : entries) {
        if (entry.tag == 258) entry.value = bitsOffset;
        if (entry.tag == 273) entry.value = pixelOffset;
    }
    std::vector<uint8_t> dng(pixelOffset + bytes, 0);
    auto put16 = [&](size_t at, uint16_t value) {
        dng[at] = uint8_t(value); dng[at + 1] = uint8_t(value >> 8);
    };
    auto put32 = [&](size_t at, uint32_t value) {
        put16(at, uint16_t(value)); put16(at + 2, uint16_t(value >> 16));
    };
    dng[0] = 'I'; dng[1] = 'I'; put16(2, 42); put32(4, 8);
    put16(8, uint16_t(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t at = 10 + i * 12;
        put16(at, entries[i].tag); put16(at + 2, entries[i].type);
        put32(at + 4, entries[i].count); put32(at + 8, entries[i].value);
    }
    for (int component = 0; component < 3; ++component)
        put16(bitsOffset + size_t(component) * 2u, 16);
    std::memcpy(dng.data() + pixelOffset, payload, bytes);
    return dng;
}

void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& message) {
    const auto text = message.toLocal8Bit();
    switch (type) {
    case QtDebugMsg: spdlog::debug("Qt: {}", text.constData()); break;
    case QtInfoMsg: spdlog::info("Qt: {}", text.constData()); break;
    case QtWarningMsg: spdlog::warn("Qt: {}", text.constData()); break;
    case QtCriticalMsg: spdlog::error("Qt: {}", text.constData()); break;
    case QtFatalMsg: spdlog::critical("Qt: {}", text.constData()); std::abort();
    }
}

#ifdef __APPLE__
int crashLogFd = -1;
void crashHandler(int signal) {
    const char header[] = "\n==== MotionCamFuse crash ====\n";
    if (crashLogFd >= 0) ::write(crashLogFd, header, sizeof(header) - 1);
    motioncam::debug::dumpCrashContext(crashLogFd >= 0 ? crashLogFd : STDERR_FILENO);
    void* stack[64];
    const int count = ::backtrace(stack, 64);
    ::backtrace_symbols_fd(stack, count, crashLogFd >= 0 ? crashLogFd : STDERR_FILENO);
    if (crashLogFd >= 0) ::fsync(crashLogFd);
    _Exit(128 + signal);
}
void installCrashHandler() {
    const QString directory = QDir::home().filePath("Library/Logs/MotionCam Tools");
    QDir().mkpath(directory);
    crashLogFd = ::open(QDir(directory).filePath("crash.txt").toUtf8().constData(),
                        O_CREAT | O_WRONLY | O_APPEND, 0644);
    struct sigaction action {};
    action.sa_handler = crashHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_RESETHAND;
    for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
        sigaction(signal, &action, nullptr);
    std::set_terminate([] { crashHandler(SIGABRT); });
}

bool platformReady() {
    const QStringList candidates{"/Library/Filesystems/macfuse.fs",
        "/Library/Frameworks/macfuse.framework", "/Library/Extensions/macfuse.kext",
        "/Library/Frameworks/fuse_t.framework"};
    for (const auto& path : candidates) if (QFileInfo::exists(path)) return true;
    QMessageBox prompt(QMessageBox::Warning, "macFUSE Not Available",
        "macFUSE is not available. You can continue using thumbnails, the gallery, "
        "and finalization, but virtual DNG folders will not be mounted.",
        QMessageBox::NoButton);
    auto* download = prompt.addButton("Open macFUSE Download", QMessageBox::AcceptRole);
    auto* privacy = prompt.addButton("Open Privacy & Security", QMessageBox::ActionRole);
    auto* continueButton = prompt.addButton("Continue Without Mounting", QMessageBox::RejectRole);
    prompt.setDefaultButton(download);
    prompt.exec();
    if (prompt.clickedButton() == download)
        QDesktopServices::openUrl(QUrl("https://macfuse.github.io/"));
    else if (prompt.clickedButton() == privacy)
        QDesktopServices::openUrl(QUrl(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Security"));
    if (prompt.clickedButton() == continueButton) {
        QSettings().setValue("fuseMountingEnabled", false);
        return true;
    }
    return false;
}
#elif defined(_WIN32)
bool platformReady() {
    HMODULE library = LoadLibraryW(L"projectedfslib.dll");
    if (library) { FreeLibrary(library); return true; }
    const QString script = QDir(QCoreApplication::applicationDirPath()).filePath("Enable_ProjFS.bat");
    QMessageBox prompt(QMessageBox::Warning, "ProjectedFS Not Available",
        "Windows Projected File System is not available. You can continue using thumbnails, "
        "the gallery, and finalization, but virtual DNG folders will not be mounted.",
        QMessageBox::NoButton);
    auto* enable = prompt.addButton("Enable ProjectedFS (Admin)", QMessageBox::AcceptRole);
    auto* openFolder = prompt.addButton("Open Setup Folder", QMessageBox::ActionRole);
    auto* continueButton = prompt.addButton("Continue Without Mounting", QMessageBox::RejectRole);
    prompt.setDefaultButton(enable);
    prompt.exec();
    if (prompt.clickedButton() == openFolder) {
        QProcess::startDetached("explorer.exe",
            {QDir::toNativeSeparators(QFileInfo(script).absolutePath())});
        return false;
    }
    if (prompt.clickedButton() == enable) {
        if (!QFileInfo::exists(script)) {
            QMessageBox::critical(nullptr, "ProjectedFS Setup", "Enable_ProjFS.bat was not found.");
        } else {
            const QString command = QString("Start-Process -FilePath '%1' -Verb RunAs")
                                        .arg(QDir::toNativeSeparators(script));
            QProcess::startDetached("powershell.exe",
                {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command});
        }
    }
    if (prompt.clickedButton() == continueButton) {
        QSettings().setValue("fuseMountingEnabled", false);
        return true;
    }
    return false;
}
#endif
bool benchmarkDirectLogSingleScan(
        const uint8_t* fullJpeg, size_t fullBytes,
        const std::vector<uint16_t>& source, uint32_t width,
        uint32_t height, uint32_t channels) {
    size_t restartIntervals = 0, restartMarkers = 0;
    for (size_t i = 0; i + 1u < fullBytes; ++i) {
        if (fullJpeg[i] != 0xffu) continue;
        if (fullJpeg[i + 1u] == 0xddu) ++restartIntervals;
        if (fullJpeg[i + 1u] >= 0xd0u && fullJpeg[i + 1u] <= 0xd7u)
            ++restartMarkers;
    }
    std::fprintf(stderr,
        "DirectLog single-scan JPEG: channels=%u bytes=%zu "
        "dri_markers=%zu restart_markers=%zu\n",
        channels, fullBytes, restartIntervals, restartMarkers);
    auto decode = [&](const uint8_t* jpeg, size_t bytes,
                      const std::vector<uint16_t>& expected,
                      uint32_t imageWidth, uint32_t imageHeight,
                      const char* label) {
        lj92 cpuDecoder = nullptr;
        int cpuWidth = 0, cpuHeight = 0, cpuBits = 0, cpuChannels = 0;
        if (lj92_open(&cpuDecoder, const_cast<uint8_t*>(jpeg), int(bytes),
                      &cpuWidth, &cpuHeight, &cpuBits, &cpuChannels) !=
            LJ92_ERROR_NONE) return false;
        std::vector<uint16_t> cpuPixels(expected.size());
        const auto cpuStarted = std::chrono::steady_clock::now();
        const bool cpuOkay = cpuWidth == int(imageWidth) &&
            cpuHeight == int(imageHeight) && cpuBits == 16 &&
            cpuChannels == int(channels) &&
            lj92_decode(cpuDecoder, cpuPixels.data(),
                        imageWidth * channels, 0, nullptr, 0) ==
                LJ92_ERROR_NONE && cpuPixels == expected;
        const double cpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStarted).count();
        lj92_close(cpuDecoder);
        if (!cpuOkay) return false;
        const motioncam::VulkanJpegTile tile{
            jpeg, bytes, imageWidth, imageHeight, 16, channels};
        const motioncam::VulkanJpegContiguousSource contiguous{jpeg, bytes};
        std::fprintf(stderr,
            "DirectLog single-scan decode start: channels=%u image=%ux%u "
            "jpeg_bytes=%zu case=%s cpu_ms=%.3f\n",
            channels, imageWidth, imageHeight, bytes, label, cpuMs);
        const int attempts = std::strcmp(label, "crop") == 0 ? 2 : 1;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            std::vector<std::vector<uint16_t>> decoded;
            const auto started = std::chrono::steady_clock::now();
            const bool ok = motioncam::jpegDecodeTilesVulkan(
                {tile}, decoded, &contiguous);
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            const bool equal = ok && decoded.size() == 1u &&
                               decoded.front() == expected;
            std::fprintf(stderr,
                "DirectLog single-scan decode: channels=%u image=%ux%u "
                "case=%s attempt=%d gpu_ms=%.3f supported=%d equal=%d\n",
                channels, imageWidth, imageHeight, label, attempt, ms, ok, equal);
            if (!ok) return true;
            if (!equal) return false;
        }
        return true;
    };
    const char* sideSetting = std::getenv(
        "MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_SIDE");
    const uint32_t side = sideSetting
        ? static_cast<uint32_t>(std::atoi(sideSetting)) : 128u;
    if (width < side || height < side ||
        source.size() != size_t(width) * height * channels) return false;
    std::vector<uint16_t> crop(size_t(side) * side * channels);
    for (uint32_t y = 0; y < side; ++y)
        std::copy_n(source.data() + size_t(y) * width * channels,
                    size_t(side) * channels,
                    crop.data() + size_t(y) * side * channels);
    uint8_t* cropJpeg = nullptr;
    int cropBytes = 0;
    if (lj92_encode(crop.data(), side, side, 16, channels,
                    side * channels, 0, nullptr, 0,
                    &cropJpeg, &cropBytes) != LJ92_ERROR_NONE)
        return false;
    const bool cropOkay = decode(cropJpeg, size_t(cropBytes), crop,
                                 side, side, "crop");
    std::free(cropJpeg);
    return cropOkay &&
        (std::getenv("MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_SKIP_FULL") ||
         decode(fullJpeg, fullBytes, source, width, height, "full"));
}

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
bool benchmarkResidentJpegRoundtrip(
    const void* gpuHandle, const void* texture,
    const std::vector<uint16_t>& source, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight) {
    auto gpu = static_cast<pl_gpu>(const_cast<void*>(gpuHandle));
    struct Release {
        const void* gpu;
        ~Release() { motioncam::releaseVulkanJpegDecodeDevice(gpu); }
    } release{gpuHandle};
    double coldMs = 0.0;
    std::array<double, 5> warmMs{};
    for (int attempt = 0; attempt < 7; ++attempt) {
        bool equal = true;
        const auto started = std::chrono::steady_clock::now();
        const bool encoded = motioncam::jpegEncodeTiledVulkanTextureResident(
            gpuHandle, texture, width, height, channels, tileWidth, tileHeight,
            [&](const void* device, const void* stream,
                const void* lengths, const void* table, uint32_t imageWidth,
                uint32_t imageHeight, uint32_t imageChannels,
                uint32_t blockWidth, uint32_t blockHeight, uint32_t tileCount,
                uint32_t slotWords) {
                return motioncam::jpegDecodeResidentTiledImageVulkan(
                    device, stream, lengths, table,
                    imageWidth, imageHeight, imageChannels,
                    blockWidth, blockHeight, tileCount, slotWords,
                    [&](const motioncam::VulkanDecodedJpegImage& image) {
                        if (attempt != 6) return true;
                        pl_buf_params params{};
                        params.size = source.size() * sizeof(uint16_t);
                        params.host_readable = true;
                        params.memory_type = PL_BUF_MEM_HOST;
                        pl_buf pixels = pl_buf_create(gpu, &params);
                        params.size = sizeof(uint32_t);
                        pl_buf valid = pl_buf_create(gpu, &params);
                        if (!pixels || !valid) {
                            pl_buf_destroy(gpu, &pixels);
                            pl_buf_destroy(gpu, &valid);
                            return false;
                        }
                        pl_buf_copy(gpu, pixels, 0,
                            static_cast<pl_buf>(const_cast<void*>(image.buffer)),
                            0, source.size() * sizeof(uint16_t));
                        pl_buf_copy(gpu, valid, 0,
                            static_cast<pl_buf>(const_cast<void*>(image.validationBuffer)),
                            0, sizeof(uint32_t));
                        std::vector<uint16_t> result(source.size());
                        uint32_t status = 0;
                        equal = pl_buf_read(gpu, pixels, 0, result.data(),
                                            result.size() * sizeof(uint16_t)) &&
                                pl_buf_read(gpu, valid, 0, &status,
                                            sizeof(uint32_t)) &&
                                status == 1u && result == source;
                        pl_buf_destroy(gpu, &pixels);
                        pl_buf_destroy(gpu, &valid);
                        return equal;
                    });
            });
        if (!encoded || !equal) return false;
        if (attempt < 6) {
            pl_gpu_finish(gpu);
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (attempt == 0) coldMs = elapsed;
            else warmMs[size_t(attempt - 1)] = elapsed;
        }
    }
    std::sort(warmMs.begin(), warmMs.end());
    std::fprintf(stderr,
        "DirectLog GPU resident JPEG roundtrip: channels=%u tile=%ux%u cold_ms=%.3f warm_ms=%.3f warm_min_ms=%.3f warm_max_ms=%.3f equal=1\n",
        channels, tileWidth, tileHeight, coldMs, warmMs[2],
        warmMs.front(), warmMs.back());
    return true;
}
#endif

bool benchmarkDirectLogJpegDecode(const std::vector<uint16_t>& source,
                                  uint32_t width, uint32_t height,
                                  uint32_t channels, uint32_t tileWidth,
                                  uint32_t tileHeight,
                                  const std::vector<std::vector<uint8_t>>* gpuEncoded = nullptr,
                                  double gpuEncodeMs = 0.0,
                                  const motioncam::VulkanJpegTiledPayload* gpuPayload = nullptr) {
    if (!tileWidth || !tileHeight || width % tileWidth || height % tileHeight ||
        (channels != 1u && channels != 3u) ||
        source.size() != size_t(width) * height * channels) return false;
    const uint32_t across = width / tileWidth;
    const uint32_t tileCount = across * (height / tileHeight);
    if (gpuEncoded && gpuEncoded->size() != tileCount) return false;
    if (gpuPayload && (gpuPayload->offsets.size() != tileCount ||
                       gpuPayload->lengths.size() != tileCount)) return false;
    std::vector<std::vector<uint8_t>> encodedOwned;
    if (!gpuEncoded && !gpuPayload) encodedOwned.resize(tileCount);
    const auto& encoded = gpuEncoded ? *gpuEncoded : encodedOwned;
    std::vector<motioncam::VulkanJpegTile> tiles(tileCount);
    const auto started = std::chrono::steady_clock::now();
    std::atomic_uint32_t nextEncodeTile{0};
    std::atomic_bool encodedOkay{true};
    std::vector<std::thread> encodeWorkers;
    if (!gpuEncoded && !gpuPayload) {
      const unsigned encodeThreads = std::min(
          {24u, tileCount, std::max(1u, std::thread::hardware_concurrency())});
      for (unsigned worker = 0; worker < encodeThreads; ++worker)
          encodeWorkers.emplace_back([&] {
            std::vector<uint16_t> samples(size_t(tileWidth) * tileHeight * channels);
            for (uint32_t tile; (tile = nextEncodeTile.fetch_add(1)) < tileCount;) {
                const uint32_t ox = (tile % across) * tileWidth;
                const uint32_t oy = (tile / across) * tileHeight;
                for (uint32_t y = 0; y < tileHeight; ++y)
                    std::copy_n(source.data() +
                                    (size_t(oy + y) * width + ox) * channels,
                                size_t(tileWidth) * channels,
                                samples.data() + size_t(y) * tileWidth * channels);
                uint8_t* bytes = nullptr;
                int count = 0;
                if (lj92_encode(samples.data(), tileWidth, tileHeight, 16,
                                channels, tileWidth * channels, 0, nullptr, 0,
                                &bytes, &count) != LJ92_ERROR_NONE ||
                    !bytes || count <= 0) {
                    std::free(bytes);
                    encodedOkay = false;
                    continue;
                }
                encodedOwned[tile].assign(bytes, bytes + count);
                std::free(bytes);
            }
          });
      for (auto& worker : encodeWorkers) worker.join();
    }
    if (!encodedOkay) return false;
    const auto encodedAt = std::chrono::steady_clock::now();
    std::vector<uint8_t> contiguousBytes;
    const uint8_t* contiguousData = nullptr;
    size_t contiguousSize = 0;
    if (gpuPayload) {
        contiguousData = gpuPayload->bytes.data();
        contiguousSize = gpuPayload->bytes.size();
        for (uint32_t tile = 0; tile < tileCount; ++tile) {
            const size_t offset = gpuPayload->offsets[tile];
            const size_t count = gpuPayload->lengths[tile];
            if (offset > contiguousSize || count > contiguousSize - offset)
                return false;
            tiles[tile] = {contiguousData + offset, count,
                           tileWidth, tileHeight, 16, channels};
        }
    } else {
        size_t totalBytes = 0;
        for (const auto& tile : encoded) totalBytes += tile.size();
        contiguousBytes.reserve(totalBytes);
        for (const auto& tile : encoded)
            contiguousBytes.insert(contiguousBytes.end(), tile.begin(), tile.end());
        size_t offset = 0;
        for (uint32_t tile = 0; tile < tileCount; ++tile) {
            tiles[tile] = {contiguousBytes.data() + offset,
                           encoded[tile].size(), tileWidth, tileHeight,
                           16, channels};
            offset += encoded[tile].size();
        }
        contiguousData = contiguousBytes.data();
        contiguousSize = contiguousBytes.size();
    }
    const motioncam::VulkanJpegContiguousSource contiguous{
        contiguousData, contiguousSize};
    const auto prepared = std::chrono::steady_clock::now();
    std::vector<uint8_t> gpuJpeg;
    const void* residentGpu = nullptr;
    struct ReleaseJpegDevice {
        const void*& gpu;
        ~ReleaseJpegDevice() {
            if (gpu) motioncam::releaseVulkanJpegFullEncodeDevice(gpu);
        }
    } releaseJpegDevice{residentGpu};
    double decodeMs[2]{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = motioncam::jpegDecodeTiledImageVulkanValidated(
            tiles, width, height, tileWidth, tileHeight, &contiguous);
        if (!ok) return false;
        decodeMs[attempt] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
    // Full pixel verification is outside the decode-only timing.
    const bool verifiedOnGpu = motioncam::jpegDecodeTiledImageVulkan(
            tiles, width, height, tileWidth, tileHeight,
            [&](const motioncam::VulkanDecodedJpegImage& image) {
                residentGpu = image.gpu;
                return motioncam::jpegEncodeFullyVulkanPackedBufferBorrowed(
                    image.gpu, image.buffer, image.width, image.height,
                    image.channels, 16, image.wordsPerRow,
                    [&](const uint8_t* bytes, size_t count) {
                        gpuJpeg.assign(bytes, bytes + count);
                        return true;
                    }, image.validationBuffer);
            }, &contiguous);
    if (!verifiedOnGpu) return false;
    lj92 gpuDecoder = nullptr;
    int jpegWidth = 0, jpegHeight = 0, bits = 0, components = 0;
    const bool opened = lj92_open(&gpuDecoder, gpuJpeg.data(),
                                  int(gpuJpeg.size()), &jpegWidth, &jpegHeight,
                                  &bits, &components) == LJ92_ERROR_NONE;
    std::vector<uint16_t> gpuPixels(source.size());
    const bool equal = opened && jpegWidth == int(width) &&
        jpegHeight == int(height) && bits == 16 &&
        components == int(channels) &&
        lj92_decode(gpuDecoder, gpuPixels.data(), width * channels, 0,
                    nullptr, 0) == LJ92_ERROR_NONE && gpuPixels == source;
    if (gpuDecoder) lj92_close(gpuDecoder);
    if (!equal) return false;
    std::vector<uint16_t> cpuPixels(source.size());
    std::atomic_uint32_t next{0};
    std::atomic_bool cpuOkay{true};
    const auto cpuStart = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 8; ++worker)
        workers.emplace_back([&] {
            for (uint32_t tile; (tile = next.fetch_add(1)) < tileCount;) {
                lj92 decoder = nullptr;
                int w = 0, h = 0, depth = 0, components = 0;
                if (lj92_open(&decoder,
                              const_cast<uint8_t*>(tiles[tile].bytes),
                              int(tiles[tile].size), &w, &h, &depth,
                              &components) != LJ92_ERROR_NONE) {
                    cpuOkay = false;
                    continue;
                }
                std::vector<uint16_t> pixels(size_t(tileWidth) * tileHeight * channels);
                const bool decoded = w == int(tileWidth) && h == int(tileHeight) &&
                    depth == 16 && components == int(channels) &&
                    lj92_decode(decoder, pixels.data(), tileWidth * channels,
                                0, nullptr, 0) == LJ92_ERROR_NONE;
                lj92_close(decoder);
                if (!decoded) { cpuOkay = false; continue; }
                const uint32_t ox = (tile % across) * tileWidth;
                const uint32_t oy = (tile / across) * tileHeight;
                for (uint32_t y = 0; y < tileHeight; ++y)
                    std::copy_n(pixels.data() + size_t(y) * tileWidth * channels,
                                size_t(tileWidth) * channels,
                                cpuPixels.data() +
                                    (size_t(oy + y) * width + ox) * channels);
            }
        });
    for (auto& worker : workers) worker.join();
    const auto cpuDecoded = std::chrono::steady_clock::now();
    if (!cpuOkay || cpuPixels != source) return false;
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr,
        "DirectLog JPEG transfer benchmark: encoder=%s channels=%u size=%ux%u tile=%ux%u tiles=%u tile_encode_ms=%.3f prepare_ms=%.3f gpu_decode_cold_ms=%.3f gpu_decode_warm_ms=%.3f readback_reupload_roundtrip_ms=%.3f cpu_decode_ms=%.3f input_jpeg_bytes=%zu equal=%d\n",
        (gpuEncoded || gpuPayload) ? "gpu" : "cpu",
        channels, width, height, tileWidth, tileHeight, tileCount,
        (gpuEncoded || gpuPayload) ? gpuEncodeMs : ms(started, encodedAt),
        ms(started, prepared),
        decodeMs[0], decodeMs[1],
        ((gpuEncoded || gpuPayload) ? gpuEncodeMs : ms(started, encodedAt)) +
            decodeMs[1],
        ms(cpuStart, cpuDecoded),
        contiguousSize, equal);
    return true;
}
} // namespace

int main(int argc, char *argv[])
{
    const auto processStarted = std::chrono::steady_clock::now();
#ifndef _WIN32
    // Pipe consumers such as the gallery FFmpeg process can exit between a
    // state check and a buffered write. Let QProcess report EPIPE instead of
    // allowing the process-wide default SIGPIPE action to terminate the app.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    SingleApplication app(argc, argv);
    const auto applicationConstructed = std::chrono::steady_clock::now();

#ifdef __APPLE__
    installCrashHandler();
#endif

    // Set application properties
    app.setApplicationName("MotionCam Fuse");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("MotionCam");
#ifdef __APPLE__
    app.setWindowIcon(QIcon(":/assets/app_icon_mac.png"));
    QFont appFont = app.font();
    if (appFont.pointSizeF() > 0) {
        appFont.setPointSizeF(appFont.pointSizeF() + 2.0);
        app.setFont(appFont);
    }
#else
    app.setWindowIcon(QIcon(":/assets/app_icon.png"));
#endif

    // Load theme
    QFile themeFile(":qdarkstyle/dark/darkstyle.qss");

    if (themeFile.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&themeFile);
        app.setStyleSheet(ts.readAll());
    }

    // Parse command line arguments
    QCommandLineParser parser;

    parser.setApplicationDescription("MotionCam Fuse");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add file option
    QCommandLineOption fileOption(QStringList() << "f" << "file",
                                  "Mount file on startup",
                                  "filename");
    QCommandLineOption galleryPerfOption(
        QStringList() << "gallery-perf-session",
        "Load a session and automate gallery playback diagnostics",
        "session");
    QCommandLineOption galleryPerfPlaybackOption(
        QStringList() << "gallery-perf-playback-ms",
        "Playback sample duration before and after each seek",
        "milliseconds", "3000");
    QCommandLineOption galleryPerfThumbnailsOption(
        QStringList() << "gallery-perf-thumbnails",
        "Generate thumbnails concurrently during the cold gallery startup (on/off)",
        "mode", "on");
    QCommandLineOption directLogJpegBenchmarkOption(
        QStringList() << "directlog-jpeg-benchmark",
        "Compare remosaiced DirectLog GPU JPEG with raw pixel download",
        "video");

    parser.addOption(fileOption);
    parser.addOption(galleryPerfOption);
    parser.addOption(galleryPerfPlaybackOption);
    parser.addOption(galleryPerfThumbnailsOption);
    parser.addOption(directLogJpegBenchmarkOption);
    parser.process(app);

    if (parser.isSet(directLogJpegBenchmarkOption)) {
        try {
            motioncam::DirectLogDecoder decoder(
                parser.value(directLogJpegBenchmarkOption).toStdString());
            const auto& info = decoder.getVideoInfo();
            if (!decoder.canExtractFrameTexture() || info.width <= 0 ||
                info.height <= 0) {
                std::fprintf(stderr, "DirectLog GPU texture unavailable\n");
                return 2;
            }
            const std::array<uint8_t, 4> phase{0, 1, 1, 2};
            std::vector<uint16_t> deferred;
            const auto decodedStart = std::chrono::steady_clock::now();
            if (!decoder.extractFrameBayerDeferred(0, deferred, phase,
                                                   info.width, info.height))
                return 3;
            const auto decodedEnd = std::chrono::steady_clock::now();
            const void* gpu = nullptr;
            const void* texture = nullptr;
            if (!decoder.pendingGpuTexture(gpu, texture)) return 4;
            uint8_t *jpeg = nullptr, *warmJpeg = nullptr;
            int jpegBytes = 0, warmJpegBytes = 0;
            const auto encodeStart = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkanTexture(
                    gpu, texture, info.width, info.height,
                    &jpeg, &jpegBytes)) return 5;
            const auto firstEnd = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkanTexture(
                    gpu, texture, info.width, info.height,
                    &warmJpeg, &warmJpegBytes)) {
                std::free(jpeg);
                return 6;
            }
            const auto warmEnd = std::chrono::steady_clock::now();
            std::vector<uint16_t> raw(static_cast<size_t>(info.width) * info.height);
            const bool downloaded = decoder.downloadPendingGpu(raw.data(), raw.size());
            const auto rawEnd = std::chrono::steady_clock::now();
            auto rawDng = benchmarkCfaDng(
                reinterpret_cast<const uint8_t*>(raw.data()),
                raw.size() * sizeof(uint16_t), info.width, info.height, false);
            const auto rawDngEnd = std::chrono::steady_clock::now();
            auto jpegDng = benchmarkCfaDng(
                warmJpeg, warmJpegBytes, info.width, info.height, true);
            const auto jpegDngEnd = std::chrono::steady_clock::now();
            auto stagedJpegDng = rawDng;
            const auto stagedJpegStart = std::chrono::steady_clock::now();
            const bool stagedJpegOk =
                motioncam::DNGDecoder::compressLosslessJPEG(stagedJpegDng);
            const auto stagedJpegEnd = std::chrono::steady_clock::now();
            auto stagedWarmJpegDng = rawDng;
            const auto stagedWarmStart = std::chrono::steady_clock::now();
            const bool stagedWarmOk =
                motioncam::DNGDecoder::compressLosslessJPEG(stagedWarmJpegDng);
            const auto stagedWarmEnd = std::chrono::steady_clock::now();
            lj92 handle = nullptr;
            int width = 0, height = 0, bits = 0, channels = 0;
            const bool opened = lj92_open(&handle, warmJpeg, warmJpegBytes,
                                          &width, &height, &bits, &channels) ==
                LJ92_ERROR_NONE;
            std::vector<uint16_t> decoded(raw.size());
            const auto cfaDecodeStart = std::chrono::steady_clock::now();
            const bool cfaDecoded = opened && width == info.width &&
                height == info.height && channels == 1 && bits == 16 &&
                lj92_decode(handle, decoded.data(), width, 0, nullptr, 0) ==
                    LJ92_ERROR_NONE;
            const auto cfaDecodeEnd = std::chrono::steady_clock::now();
            motioncam::DecodedDNGImage rawDngImage, jpegDngImage,
                                       stagedJpegImage;
            const bool dngEqual = !rawDng.empty() && !jpegDng.empty() &&
                motioncam::DNGDecoder::decodeImageBorrowed(rawDng, rawDngImage,
                                                            false, false) &&
                motioncam::DNGDecoder::decodeImageBorrowed(jpegDng, jpegDngImage,
                                                            false, false) &&
                stagedJpegOk && stagedWarmOk &&
                motioncam::DNGDecoder::decodeImageBorrowed(
                    stagedJpegDng, stagedJpegImage, false, false) &&
                rawDngImage.samples == jpegDngImage.samples &&
                rawDngImage.samples == stagedJpegImage.samples;
            const bool equal = downloaded && cfaDecoded && raw == decoded &&
                jpegBytes == warmJpegBytes &&
                std::memcmp(jpeg, warmJpeg, jpegBytes) == 0 && dngEqual;
            if (handle) lj92_close(handle);
            const char* singleScanSetting = std::getenv(
                "MOTIONCAM_DIRECTLOG_JPEG_SINGLE_SCAN_BENCHMARK");
            if (singleScanSetting && std::strcmp(singleScanSetting, "rgb") != 0 &&
                !benchmarkDirectLogSingleScan(
                    warmJpeg, size_t(warmJpegBytes), raw,
                    info.width, info.height, 1u)) return 15;
            std::free(jpeg);
            std::free(warmJpeg);
            const auto ms = [](auto start, auto end) {
                return std::chrono::duration<double, std::milli>(end-start).count();
            };
            std::fprintf(stderr,
                "DirectLog GPU CFA JPEG benchmark: width=%d height=%d channels=1 decode_ms=%.3f jpeg_first_ms=%.3f jpeg_warm_ms=%.3f full_cpu_decode_ms=%.3f full_roundtrip_estimate_ms=%.3f raw_download_ms=%.3f raw_dng_ms=%.3f direct_jpeg_dng_ms=%.3f staged_jpeg_dng_first_ms=%.3f staged_jpeg_dng_warm_ms=%.3f raw_dng_bytes=%zu direct_jpeg_dng_bytes=%zu staged_jpeg_dng_bytes=%zu equal=%d\n",
                info.width, info.height,
                ms(decodedStart, decodedEnd), ms(encodeStart, firstEnd),
                ms(firstEnd, warmEnd), ms(cfaDecodeStart, cfaDecodeEnd),
                ms(firstEnd, warmEnd) + ms(cfaDecodeStart, cfaDecodeEnd),
                ms(warmEnd, rawEnd),
                ms(rawEnd, rawDngEnd), ms(rawDngEnd, jpegDngEnd),
                ms(stagedJpegStart, stagedJpegEnd),
                ms(stagedWarmStart, stagedWarmEnd), rawDng.size(),
                jpegDng.size(), stagedJpegDng.size(), equal);
            if (!equal) return 7;
            if (std::getenv("MOTIONCAM_DIRECTLOG_JPEG_GPU_TILED_BENCHMARK")) {
                const char* widthSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_WIDTH");
                const char* heightSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_HEIGHT");
                const uint32_t tileWidth = widthSetting
                    ? uint32_t(std::atoi(widthSetting)) : 16u;
                const uint32_t tileHeight = heightSetting
                    ? uint32_t(std::atoi(heightSetting)) : 64u;
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
                if (!benchmarkResidentJpegRoundtrip(
                        gpu, texture, raw, info.width, info.height, 1u,
                        tileWidth, tileHeight)) return 26;
#endif
                motioncam::VulkanJpegTiledPayload gpuPayload;
                const auto tileColdStart = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeTiledVulkanTexture(
                        gpu, texture, info.width, info.height, 1u,
                        tileWidth, tileHeight,
                        gpuPayload)) return 17;
                const auto tileWarmStart = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeTiledVulkanTexture(
                        gpu, texture, info.width, info.height, 1u,
                        tileWidth, tileHeight,
                        gpuPayload)) return 18;
                const auto tileWarmEnd = std::chrono::steady_clock::now();
                std::fprintf(stderr,
                    "DirectLog GPU CFA tiled encode: cold_ms=%.3f warm_ms=%.3f\n",
                    ms(tileColdStart, tileWarmStart),
                    ms(tileWarmStart, tileWarmEnd));
                if (!benchmarkDirectLogJpegDecode(
                        raw, info.width, info.height, 1u,
                        tileWidth, tileHeight,
                        nullptr, ms(tileWarmStart, tileWarmEnd),
                        &gpuPayload)) return 19;
                std::vector<std::vector<uint8_t>> gpuTiles(gpuPayload.offsets.size());
                for (size_t tile = 0; tile < gpuTiles.size(); ++tile)
                    gpuTiles[tile].assign(
                        gpuPayload.bytes.begin() + gpuPayload.offsets[tile],
                        gpuPayload.bytes.begin() + gpuPayload.offsets[tile] +
                            gpuPayload.lengths[tile]);
                auto tiledDng = rawDng;
                motioncam::DecodedDNGImage tiledImage;
                const auto attachStart = std::chrono::steady_clock::now();
                const bool attached = motioncam::DNGDecoder::attachLosslessJPEGTiles(
                    tiledDng, gpuTiles, tileWidth, tileHeight);
                const auto attachEnd = std::chrono::steady_clock::now();
                if (!attached ||
                    !motioncam::DNGDecoder::decodeImageBorrowed(
                        tiledDng, tiledImage, false, false) ||
                    tiledImage.samples != raw) return 24;
                std::fprintf(stderr,
                    "DirectLog GPU CFA tiled DNG: attach_ms=%.3f bytes=%zu equal=1\n",
                    ms(attachStart, attachEnd), tiledDng.size());
            }

            const auto rgbDecodeStart = std::chrono::steady_clock::now();
            if (!decoder.extractFrameTextureDeferred(
                    0, info.width, info.height, true)) return 9;
            const auto rgbDecodeEnd = std::chrono::steady_clock::now();
            if (!decoder.pendingGpuTexture(gpu, texture)) return 10;
            uint8_t *rgbFirst = nullptr, *rgbWarm = nullptr, *rgbCpu = nullptr;
            int rgbFirstBytes = 0, rgbWarmBytes = 0, rgbCpuBytes = 0;
            const auto rgbEncodeStart = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkanTexture(
                    gpu, texture, info.width, info.height, 3, 16,
                    &rgbFirst, &rgbFirstBytes)) return 11;
            const auto rgbFirstEnd = std::chrono::steady_clock::now();
            if (!motioncam::jpegEncodeFullyVulkanTexture(
                    gpu, texture, info.width, info.height, 3, 16,
                    &rgbWarm, &rgbWarmBytes)) {
                std::free(rgbFirst);
                return 12;
            }
            const auto rgbWarmEnd = std::chrono::steady_clock::now();
            std::vector<uint16_t> rgbRaw(
                static_cast<size_t>(info.width) * info.height * 3u);
            const bool rgbDownloaded = decoder.downloadPendingGpu(
                rgbRaw.data(), rgbRaw.size());
            const auto rgbDownloadEnd = std::chrono::steady_clock::now();
            lj92 rgbDecoder = nullptr;
            int rgbWidth = 0, rgbHeight = 0, rgbBits = 0, rgbChannels = 0;
            std::vector<uint16_t> rgbDecoded(rgbRaw.size());
            const auto rgbCpuDecodeStart = std::chrono::steady_clock::now();
            const bool rgbOpened = lj92_open(
                &rgbDecoder, rgbWarm, rgbWarmBytes, &rgbWidth, &rgbHeight,
                &rgbBits, &rgbChannels) == LJ92_ERROR_NONE;
            const bool rgbDecodedOkay = rgbOpened &&
                rgbWidth == info.width && rgbHeight == info.height &&
                rgbBits == 16 && rgbChannels == 3 &&
                lj92_decode(rgbDecoder, rgbDecoded.data(), info.width * 3,
                            0, nullptr, 0) == LJ92_ERROR_NONE;
            if (rgbDecoder) lj92_close(rgbDecoder);
            const auto rgbCpuDecodeEnd = std::chrono::steady_clock::now();
            const bool rgbCpuOk = rgbDownloaded &&
                lj92_encode(rgbRaw.data(), info.width, info.height, 16, 3,
                            info.width * 3, 0, nullptr, 0,
                            &rgbCpu, &rgbCpuBytes) == LJ92_ERROR_NONE;
            const auto rgbCpuEnd = std::chrono::steady_clock::now();
            const bool rgbEqual = rgbCpuOk && rgbDecodedOkay &&
                rgbDecoded == rgbRaw &&
                rgbCpuBytes == rgbFirstBytes && rgbCpuBytes == rgbWarmBytes &&
                std::memcmp(rgbCpu, rgbFirst, rgbCpuBytes) == 0 &&
                std::memcmp(rgbCpu, rgbWarm, rgbCpuBytes) == 0;
            if (singleScanSetting && std::strcmp(singleScanSetting, "cfa") != 0 &&
                !benchmarkDirectLogSingleScan(
                    rgbWarm, size_t(rgbWarmBytes), rgbRaw,
                    info.width, info.height, 3u)) return 16;
            std::fprintf(stderr,
                "DirectLog GPU RGB JPEG benchmark: width=%d height=%d "
                "channels=3 decode_ms=%.3f jpeg_first_ms=%.3f "
                "jpeg_warm_ms=%.3f full_cpu_decode_ms=%.3f "
                "full_roundtrip_estimate_ms=%.3f raw_download_ms=%.3f cpu_jpeg_ms=%.3f "
                "jpeg_bytes=%d byte_equal=%d\n",
                info.width, info.height, ms(rgbDecodeStart, rgbDecodeEnd),
                ms(rgbEncodeStart, rgbFirstEnd), ms(rgbFirstEnd, rgbWarmEnd),
                ms(rgbCpuDecodeStart, rgbCpuDecodeEnd),
                ms(rgbFirstEnd, rgbWarmEnd) +
                    ms(rgbCpuDecodeStart, rgbCpuDecodeEnd),
                ms(rgbWarmEnd, rgbDownloadEnd),
                ms(rgbDownloadEnd, rgbCpuEnd), rgbWarmBytes, rgbEqual);
            if (!rgbEqual) return 20;
            if (std::getenv("MOTIONCAM_DIRECTLOG_JPEG_GPU_TILED_BENCHMARK")) {
                const char* widthSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_WIDTH");
                const char* heightSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_HEIGHT");
                const uint32_t tileWidth = widthSetting
                    ? uint32_t(std::atoi(widthSetting)) : 8u;
                const uint32_t tileHeight = heightSetting
                    ? uint32_t(std::atoi(heightSetting)) : 128u;
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
                if (!benchmarkResidentJpegRoundtrip(
                        gpu, texture, rgbRaw, info.width, info.height, 3u,
                        tileWidth, tileHeight)) return 27;
#endif
                motioncam::VulkanJpegTiledPayload gpuPayload;
                const auto tileColdStart = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeTiledVulkanTexture(
                        gpu, texture, info.width, info.height, 3u,
                        tileWidth, tileHeight,
                        gpuPayload)) return 21;
                const auto tileWarmStart = std::chrono::steady_clock::now();
                if (!motioncam::jpegEncodeTiledVulkanTexture(
                        gpu, texture, info.width, info.height, 3u,
                        tileWidth, tileHeight,
                        gpuPayload)) return 22;
                const auto tileWarmEnd = std::chrono::steady_clock::now();
                std::fprintf(stderr,
                    "DirectLog GPU RGB tiled encode: cold_ms=%.3f warm_ms=%.3f\n",
                    ms(tileColdStart, tileWarmStart),
                    ms(tileWarmStart, tileWarmEnd));
                if (!benchmarkDirectLogJpegDecode(
                        rgbRaw, info.width, info.height, 3u,
                        tileWidth, tileHeight,
                        nullptr, ms(tileWarmStart, tileWarmEnd),
                        &gpuPayload)) return 23;
                std::vector<std::vector<uint8_t>> gpuTiles(gpuPayload.offsets.size());
                for (size_t tile = 0; tile < gpuTiles.size(); ++tile)
                    gpuTiles[tile].assign(
                        gpuPayload.bytes.begin() + gpuPayload.offsets[tile],
                        gpuPayload.bytes.begin() + gpuPayload.offsets[tile] +
                            gpuPayload.lengths[tile]);
                auto tiledDng = benchmarkRgbDng(
                    reinterpret_cast<const uint8_t*>(rgbRaw.data()),
                    rgbRaw.size() * sizeof(uint16_t),
                    info.width, info.height, false);
                motioncam::DecodedDNGImage tiledImage;
                const auto attachStart = std::chrono::steady_clock::now();
                const bool attached = motioncam::DNGDecoder::attachLosslessJPEGTiles(
                    tiledDng, gpuTiles, tileWidth, tileHeight);
                const auto attachEnd = std::chrono::steady_clock::now();
                if (!attached ||
                    !motioncam::DNGDecoder::decodeImageBorrowed(
                        tiledDng, tiledImage, false, false) ||
                    tiledImage.samples != rgbRaw) return 25;
                std::fprintf(stderr,
                    "DirectLog GPU RGB tiled DNG: attach_ms=%.3f bytes=%zu equal=1\n",
                    ms(attachStart, attachEnd), tiledDng.size());
            }
            const auto rawRgbDngStart = std::chrono::steady_clock::now();
            auto rawRgbDng = benchmarkRgbDng(
                reinterpret_cast<const uint8_t*>(rgbRaw.data()),
                rgbRaw.size() * sizeof(uint16_t),
                info.width, info.height, false);
            const auto rawRgbDngEnd = std::chrono::steady_clock::now();
            const auto ownedDngStart = std::chrono::steady_clock::now();
            auto ownedRgbDng = benchmarkRgbDng(
                rgbWarm, rgbWarmBytes, info.width, info.height, true);
            const auto ownedDngEnd = std::chrono::steady_clock::now();
            std::vector<uint8_t> borrowedRgbDng;
            const auto borrowedDngStart = std::chrono::steady_clock::now();
            const bool borrowedOk =
                motioncam::jpegEncodeFullyVulkanTextureBorrowed(
                    gpu, texture, info.width, info.height, 3, 16,
                    [&](const uint8_t* payload, size_t bytes) {
                        borrowedRgbDng = benchmarkRgbDng(
                            payload, bytes, info.width, info.height, true);
                        return !borrowedRgbDng.empty();
                    });
            const auto borrowedDngEnd = std::chrono::steady_clock::now();
            motioncam::DecodedDNGImage decodedRawRgbDng, decodedRgbDng;
            const bool rawRgbEqual = !rawRgbDng.empty() &&
                motioncam::DNGDecoder::decodeImageBorrowed(
                    rawRgbDng, decodedRawRgbDng, false, false) &&
                decodedRawRgbDng.samples == rgbRaw;
            const bool rgbDngEqual = borrowedOk &&
                borrowedRgbDng == ownedRgbDng &&
                motioncam::DNGDecoder::decodeImageBorrowed(
                    borrowedRgbDng, decodedRgbDng, false, false) &&
                decodedRgbDng.samples == rgbRaw && rawRgbEqual;
            std::fprintf(stderr,
                "DirectLog GPU RGB DNG comparison: raw_wrap_ms=%.3f "
                "owned_wrap_ms=%.3f borrowed_encode_wrap_ms=%.3f "
                "raw_bytes=%zu jpeg_bytes=%zu pixel_equal=%d byte_equal=%d\n",
                ms(rawRgbDngStart, rawRgbDngEnd),
                ms(ownedDngStart, ownedDngEnd),
                ms(borrowedDngStart, borrowedDngEnd),
                rawRgbDng.size(), borrowedRgbDng.size(), rawRgbEqual,
                borrowedRgbDng == ownedRgbDng);
            if (std::getenv("MOTIONCAM_DIRECTLOG_JPEG_DECODE_BENCHMARK")) {
                const char* tileSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_SIZE");
                const uint32_t cfaTileSize = tileSetting
                    ? static_cast<uint32_t>(std::atoi(tileSetting)) : 64u;
                const uint32_t rgbTileSize = tileSetting
                    ? static_cast<uint32_t>(std::atoi(tileSetting)) : 32u;
                const char* widthSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_WIDTH");
                const char* heightSetting = std::getenv(
                    "MOTIONCAM_DIRECTLOG_JPEG_DECODE_TILE_HEIGHT");
                const uint32_t cfaTileWidth = widthSetting
                    ? static_cast<uint32_t>(std::atoi(widthSetting)) : cfaTileSize;
                const uint32_t cfaTileHeight = heightSetting
                    ? static_cast<uint32_t>(std::atoi(heightSetting)) : cfaTileSize;
                const uint32_t rgbTileWidth = widthSetting
                    ? static_cast<uint32_t>(std::atoi(widthSetting)) : rgbTileSize;
                const uint32_t rgbTileHeight = heightSetting
                    ? static_cast<uint32_t>(std::atoi(heightSetting)) : rgbTileSize;
                if (!benchmarkDirectLogJpegDecode(
                        raw, info.width, info.height, 1,
                        cfaTileWidth, cfaTileHeight) ||
                    !benchmarkDirectLogJpegDecode(
                        rgbRaw, info.width, info.height, 3,
                        rgbTileWidth, rgbTileHeight))
                    return 14;
            }
            std::free(rgbFirst);
            std::free(rgbWarm);
            std::free(rgbCpu);
            return rgbEqual && rgbDngEqual ? 0 : 13;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "DirectLog GPU JPEG benchmark failed: %s\n",
                         error.what());
            return 8;
        }
    }

    const auto commandLineParsed = std::chrono::steady_clock::now();

    // Get file parameter if provided
    QString fileToMount;
    const QString galleryPerfSession = parser.value(galleryPerfOption);
    bool galleryPerfDurationOk = false;
    const int galleryPerfPlaybackMs = parser.value(galleryPerfPlaybackOption)
        .toInt(&galleryPerfDurationOk);
    const bool galleryPerfThumbnails =
        parser.value(galleryPerfThumbnailsOption).compare("off", Qt::CaseInsensitive) != 0;
    if (!galleryPerfSession.isEmpty()) {
        qputenv("MOTIONCAM_DIRECTLOG_DIAGNOSTICS", "1");
        qputenv("MOTIONCAM_GALLERY_DIAGNOSTICS", "1");
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        spdlog::info(
            "GALLERY_PERF event=process_startup_checkpoint application_ms={:.3f} command_line_ms={:.3f} elapsed_ms={:.3f}",
            milliseconds(applicationConstructed - processStarted),
            milliseconds(commandLineParsed - applicationConstructed),
            milliseconds(commandLineParsed - processStarted));
    }

    if (parser.isSet(fileOption)) {
        fileToMount = parser.value(fileOption);
    }

    // Check if another instance is running
    if (!app.listen()) {
        if (!fileToMount.isEmpty()) {
            QString message = QString("MOUNT_FILE:%1").arg(fileToMount);
            if (app.sendMessage(message)) {
                return 0; // Successfully sent message to existing instance
            }
        }

        QMessageBox::information(nullptr,
                                 "Application Already Running",
                                 "Another instance of the application is already running.");
        return 1;
    }

#if defined(_WIN32) || defined(__APPLE__)
    {
        QSettings settings;
        if (settings.value("fuseMountingEnabled", true).toBool() && !platformReady()) return 1;
    }
#endif

    // Create main window
    const auto windowStarted = std::chrono::steady_clock::now();
    MainWindow window;
    if (!galleryPerfSession.isEmpty())
        spdlog::info(
            "GALLERY_PERF event=window_init_complete latency_ms={:.3f} process_elapsed_ms={:.3f}",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - windowStarted).count(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - processStarted).count());
    qInstallMessageHandler(messageHandler);

    // Handle messages from other instances
    QObject::connect(&app, &SingleApplication::messageReceived, &window,
        [&window](const QString &message) {
             if (message.startsWith("MOUNT_FILE:")) {
                 QString filePath = message.mid(11); // Remove "MOUNT_FILE:" prefix

                 window.mountFile(filePath);
                 window.show();
                 window.raise();
                 window.activateWindow();
             }
         });

    // Mount file if provided on startup
    if (!fileToMount.isEmpty()) {
        QTimer::singleShot(100, &window, [&window, fileToMount]() {
            window.mountFile(fileToMount);
        });
    }

    window.show();
#ifdef __APPLE__
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window,
        [&window](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) return;
            if (window.isVisible() && !window.isMinimized()) return;
            window.show();
            if (window.isMinimized()) window.showNormal();
            window.raise();
            window.activateWindow();
        });
#endif
    if (!galleryPerfSession.isEmpty()) {
        QTimer::singleShot(0, &window,
            [&window, galleryPerfSession, galleryPerfPlaybackMs, galleryPerfDurationOk,
             galleryPerfThumbnails, processStarted] {
                spdlog::info(
                    "GALLERY_PERF event=test_dispatch process_elapsed_ms={:.3f}",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - processStarted).count());
                window.startGalleryPerformanceTest(
                    galleryPerfSession,
                    galleryPerfDurationOk ? std::max(250, galleryPerfPlaybackMs) : 3000,
                    galleryPerfThumbnails);
            });
    } else if (fileToMount.isEmpty())
        QTimer::singleShot(0, &window, [&window] { window.promptToResumeSession(); });
    const int exitCode = app.exec();
    if (!galleryPerfSession.isEmpty())
        spdlog::info(
            "GALLERY_PERF event=event_loop_exit code={} process_elapsed_ms={:.3f}",
            exitCode, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - processStarted).count());
    return exitCode;
}

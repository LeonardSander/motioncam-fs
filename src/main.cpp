#include "mainwindow.h"
#include "SingleApplication.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
#include "VulkanJpegFullEncode.h"
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
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
            const bool equal = downloaded && opened && width == info.width &&
                height == info.height && channels == 1 && bits == 16 &&
                lj92_decode(handle, decoded.data(), width, 0, nullptr, 0) ==
                    LJ92_ERROR_NONE && raw == decoded &&
                jpegBytes == warmJpegBytes &&
                std::memcmp(jpeg, warmJpeg, jpegBytes) == 0 && dngEqual;
            if (handle) lj92_close(handle);
            std::free(jpeg);
            std::free(warmJpeg);
            const auto ms = [](auto start, auto end) {
                return std::chrono::duration<double, std::milli>(end-start).count();
            };
            std::fprintf(stderr,
                "DirectLog GPU CFA JPEG benchmark: width=%d height=%d channels=1 decode_ms=%.3f jpeg_first_ms=%.3f jpeg_warm_ms=%.3f raw_download_ms=%.3f raw_dng_ms=%.3f direct_jpeg_dng_ms=%.3f staged_jpeg_dng_first_ms=%.3f staged_jpeg_dng_warm_ms=%.3f raw_dng_bytes=%zu direct_jpeg_dng_bytes=%zu staged_jpeg_dng_bytes=%zu equal=%d\n",
                info.width, info.height,
                ms(decodedStart, decodedEnd), ms(encodeStart, firstEnd),
                ms(firstEnd, warmEnd), ms(warmEnd, rawEnd),
                ms(rawEnd, rawDngEnd), ms(rawDngEnd, jpegDngEnd),
                ms(stagedJpegStart, stagedJpegEnd),
                ms(stagedWarmStart, stagedWarmEnd), rawDng.size(),
                jpegDng.size(), stagedJpegDng.size(), equal);
            if (!equal) return 7;

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
            const bool rgbCpuOk = rgbDownloaded &&
                lj92_encode(rgbRaw.data(), info.width, info.height, 16, 3,
                            info.width * 3, 0, nullptr, 0,
                            &rgbCpu, &rgbCpuBytes) == LJ92_ERROR_NONE;
            const auto rgbCpuEnd = std::chrono::steady_clock::now();
            const bool rgbEqual = rgbCpuOk &&
                rgbCpuBytes == rgbFirstBytes && rgbCpuBytes == rgbWarmBytes &&
                std::memcmp(rgbCpu, rgbFirst, rgbCpuBytes) == 0 &&
                std::memcmp(rgbCpu, rgbWarm, rgbCpuBytes) == 0;
            std::fprintf(stderr,
                "DirectLog GPU RGB JPEG benchmark: width=%d height=%d "
                "channels=3 decode_ms=%.3f jpeg_first_ms=%.3f "
                "jpeg_warm_ms=%.3f raw_download_ms=%.3f cpu_jpeg_ms=%.3f "
                "jpeg_bytes=%d byte_equal=%d\n",
                info.width, info.height, ms(rgbDecodeStart, rgbDecodeEnd),
                ms(rgbEncodeStart, rgbFirstEnd), ms(rgbFirstEnd, rgbWarmEnd),
                ms(rgbWarmEnd, rgbDownloadEnd),
                ms(rgbDownloadEnd, rgbCpuEnd), rgbWarmBytes, rgbEqual);
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

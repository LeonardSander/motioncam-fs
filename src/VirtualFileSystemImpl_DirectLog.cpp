#include "VirtualFileSystemImpl_DirectLog.h"
#include "VirtualFileSystemImpl.h"
#include "DirectLogDecoder.h"
#include "DNGDecoder.h"
#include "CalibrationData.h"
#include "Utils.h"
#include "LRUCache.h"
#include "Types.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include "tinydng/tiny_dng_writer.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <QByteArray>

using motioncam::Timestamp;

namespace {

std::vector<uint8_t> packSamples(
    const std::vector<uint16_t>& samples,
    size_t samplesPerRow,
    unsigned int bitsPerSample) {
    if (samplesPerRow == 0 || samples.size() % samplesPerRow != 0)
        throw std::invalid_argument("Invalid dimensions for sample packing");
    const size_t rows = samples.size() / samplesPerRow;
    const size_t rowBytes =
        (samplesPerRow * static_cast<size_t>(bitsPerSample) + 7) / 8;
    std::vector<uint8_t> output(rows * rowBytes, 0);
    const uint16_t mask = bitsPerSample == 16
        ? 0xffffu
        : static_cast<uint16_t>((1u << bitsPerSample) - 1u);

    for (size_t row = 0; row < rows; ++row) {
        size_t bitOffset = row * rowBytes * 8;
        for (size_t column = 0; column < samplesPerRow; ++column) {
            const uint16_t sample = samples[row * samplesPerRow + column] & mask;
            for (int bit = static_cast<int>(bitsPerSample) - 1; bit >= 0; --bit) {
                output[bitOffset / 8] |= static_cast<uint8_t>(
                    ((sample >> bit) & 1u) << (7 - bitOffset % 8));
                ++bitOffset;
            }
        }
    }
    return output;
}

} // namespace

namespace motioncam {



VirtualFileSystemImpl_DirectLog::VirtualFileSystemImpl_DirectLog(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        const RenderSettings& config,
        const std::string& file,
        const std::string& baseName) :
        mCache(lruCache),
        mIoThreadPool(ioThreadPool),
        mProcessingThreadPool(processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mTypicalDngSize(0),
        mFps(0),
        mFrameRateInfo(),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mConfig(config),
        mIsHLG(false) {
    
    // Load calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    if (boost::filesystem::exists(calibPath)) {
        loadSidecarMetadata(calibPath);
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Loaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    
    // Initialize DirectLogDecoder
    try {
        mDecoder = std::make_unique<DirectLogDecoder>(mSrcPath);
        if (mCalibration && mCalibration->hasDataLevels) {
            if (mCalibration->dataLevels == "Full")
                mDecoder->setFullRangeOverride(true);
            else if (mCalibration->dataLevels == "Limited")
                mDecoder->setFullRangeOverride(false);
        }
        const auto& videoInfo = mDecoder->getVideoInfo();
        
        mWidth = videoInfo.width;
        mHeight = videoInfo.height;
        mTotalFrames = static_cast<int>(videoInfo.totalFrames);
        mPixelFormat = videoInfo.pixelFormat;
        mIsHLG = videoInfo.isHLG;
        mDroppedFrames = 0;
        mDuplicatedFrames = 0;
        
        // Calculate frame rate statistics from actual frame timestamps
        calculateFrameRateStats();
        
        spdlog::info("DirectLog video loaded: {}x{} @ {:.2f}fps (avg: {:.2f}, med: {:.2f}), {} frames, format: {}, HLG: {}", 
                     mWidth, mHeight, mFps, mFrameRateInfo.averageFrameRate, mFrameRateInfo.medianFrameRate, mTotalFrames, mPixelFormat, mIsHLG);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize DirectLogDecoder: {}", e.what());
        throw;
    }
    
    this->init();
}

VirtualFileSystemImpl_DirectLog::~VirtualFileSystemImpl_DirectLog() {
    spdlog::info("Destroying VirtualFileSystemImpl_DirectLog({})", mSrcPath);
}

void VirtualFileSystemImpl_DirectLog::init() {
    spdlog::debug("VirtualFileSystemImpl_DirectLog::init(options={})", optionsToString(mConfig.options));
    
    mFiles.clear();

#ifdef _WIN32
    Entry desktopIni;
    desktopIni.type = EntryType::FILE_ENTRY;
    desktopIni.pathParts = {};
    desktopIni.name = "desktop.ini";
    desktopIni.size = vfs::DESKTOP_INI.size();
    mFiles.push_back(desktopIni);
#endif

    const auto& frames = mDecoder->getFrames();
    if (frames.empty()) {
        return;
    }
    
    // Determine target FPS based on CFR conversion options
    bool applyCFRConversion = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
    mFps = vfs::determineCFRTarget(mFrameRateInfo, mConfig.cfrTarget, applyCFRConversion);
    
    spdlog::info("DirectLog target FPS: {:.2f} (CFR conversion: {})", mFps, applyCFRConversion);
    
    // Generate one sample DNG to determine actual file size (mimics MCRAW approach)
    if (!frames.empty()) {
        std::vector<uint16_t> sampleRgbData;
        if (mDecoder->extractFrame(0, sampleRgbData)) {
            auto sampleGainMaps = loadSidecarGainMaps(0, "gainMaps");
            auto sampleDeferredGainMaps = loadSidecarGainMaps(0, "deferredGainMaps");
            std::vector<GainMap> sampleOpcodeList2, sampleOpcodeList3;
            auto classifySampleMaps = [&](const std::vector<GainMap>& maps) {
                if (maps.empty()) return;
                if (maps.size() == 4 || (maps.size() == 1 && maps.front().channels == 4))
                    sampleOpcodeList2.insert(sampleOpcodeList2.end(), maps.begin(), maps.end());
                else if (maps.size() == 1 && maps.front().channels == 1)
                    sampleOpcodeList3.push_back(maps.front());
                else
                    throw std::runtime_error("Unsupported DirectLog gain-map layout");
            };
            if (!(mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION)) {
                classifySampleMaps(sampleGainMaps);
                classifySampleMaps(sampleDeferredGainMaps);
            } else if (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) {
                if (sampleDeferredGainMaps.size() == 1 &&
                    sampleDeferredGainMaps.front().channels == 1)
                    sampleOpcodeList3 = sampleDeferredGainMaps;
                if (sampleGainMaps.size() == 1 && sampleGainMaps.front().channels == 1)
                    sampleOpcodeList3 = sampleGainMaps;
            }
            double sampleIso = 0.0, sampleShutter = 0.0, sampleBaseline = 0.0;
            std::optional<std::array<float, 3>> sampleNeutral;
            if (mSidecarMetadata.contains("dynamic") &&
                mSidecarMetadata["dynamic"].contains("frames") &&
                !mSidecarMetadata["dynamic"]["frames"].empty()) {
                const auto& metadata = mSidecarMetadata["dynamic"]["frames"][0];
                sampleIso = metadata.value("iso", 0.0);
                sampleShutter = metadata.value("shutterSpeedSeconds", 0.0);
                sampleBaseline = metadata.value("baselineExposure", 0.0);
                if (metadata.contains("asShotNeutral") &&
                    metadata["asShotNeutral"].is_array() &&
                    metadata["asShotNeutral"].size() >= 3)
                    sampleNeutral = std::array<float, 3>{
                        metadata["asShotNeutral"][0].get<float>(),
                        metadata["asShotNeutral"][1].get<float>(),
                        metadata["asShotNeutral"][2].get<float>()};
            }
            std::vector<uint8_t> sampleDngData;
            if (convertRGBToDNG(sampleRgbData, sampleDngData, 0, frames[0].timestamp,
                                false, 0.0f, {1.0f, 1.0f, 1.0f}, sampleIso,
                                sampleShutter, sampleBaseline, sampleNeutral,
                                sampleOpcodeList2, sampleOpcodeList3)) {
                if (!DNGDecoder::setTimingMetadata(sampleDngData, mFps, 0))
                    throw std::runtime_error("Could not size DirectLog DNG timing metadata");
                mTypicalDngSize = sampleDngData.size();
                spdlog::info("DirectLog DNG size determined from sample: {} bytes ({:.2f} MB)", 
                            mTypicalDngSize, mTypicalDngSize / (1024.0 * 1024.0));
            } else {
                // Fallback to calculated estimate if conversion fails
                const auto& videoInfo = mDecoder->getVideoInfo();
                mTypicalDngSize = static_cast<size_t>(videoInfo.width) * videoInfo.height * 3 * 2 + (1024 * 1024);
                spdlog::warn("Failed to generate sample DNG, using estimated size: {} bytes", mTypicalDngSize);
            }
        }
    }
    
    // Generate file entries with CFR conversion if enabled
    int lastPts = 0;
    mDroppedFrames = 0;
    mDuplicatedFrames = 0;
    
    if (applyCFRConversion) {
        // CFR conversion: duplicate/drop frames to match target framerate
        Timestamp previousTimestamp = frames.front().timestamp;
        for (size_t i = 0; i < frames.size(); ++i) {
            int pts = vfs::getFrameNumberFromTimestamp(frames[i].timestamp, frames[0].timestamp, mFps);
            
            if (pts < lastPts) {
                mDroppedFrames += 1;
                continue;
            }

            // Hold the preceding frame through gaps, then switch to the
            // current frame at its mapped output position.
            while (lastPts < pts) {
                Entry dngEntry;
                dngEntry.type = EntryType::FILE_ENTRY;
                dngEntry.pathParts = {};
                dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
                dngEntry.size = mTypicalDngSize;
                dngEntry.userData = previousTimestamp;
                mFiles.push_back(dngEntry);
                ++lastPts;
                ++mDuplicatedFrames;
            }
            Entry dngEntry;
            dngEntry.type = EntryType::FILE_ENTRY;
            dngEntry.pathParts = {};
            dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
            dngEntry.size = mTypicalDngSize;
            dngEntry.userData = frames[i].timestamp;
            mFiles.push_back(dngEntry);
            ++lastPts;
            previousTimestamp = frames[i].timestamp;
        }
    } else {
        // No CFR conversion: use frames as-is
        for (size_t i = 0; i < frames.size(); ++i) {
            Entry dngEntry;
            dngEntry.type = EntryType::FILE_ENTRY;
            dngEntry.pathParts = {};
            dngEntry.name = vfs::constructFrameFilename(mBaseName + "-", lastPts, 6, "dng");
            dngEntry.size = mTypicalDngSize;
            dngEntry.userData = frames[i].timestamp;
            mFiles.push_back(dngEntry);
            ++lastPts;
        }
    }
    
    spdlog::info("DirectLog generated {} DNG entries (dropped: {}, duplicated: {})", 
                 mFiles.size(), mDroppedFrames, mDuplicatedFrames);
}

std::vector<Entry> VirtualFileSystemImpl_DirectLog::listFiles(const std::string& filter) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (filter.empty()) {
        return mFiles;
    }
    
    std::vector<Entry> filteredFiles;
    for (const auto& file : mFiles) {
        if (file.name.find(filter) != std::string::npos) {
            filteredFiles.push_back(file);
        }
    }
    
    return filteredFiles;
}

std::optional<Entry> VirtualFileSystemImpl_DirectLog::findEntry(const std::string& fullPath) const {
    std::lock_guard<std::mutex> lock(mMutex);
    
    for (const auto& entry : mFiles) {
        if (entry.getFullPath() == boost::filesystem::path(fullPath).relative_path()) {
            return entry;
        }
    }
    
    return std::nullopt;
}

int VirtualFileSystemImpl_DirectLog::readFile(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {
    
    if (entry.name == "desktop.ini") {
#ifdef _WIN32
        size_t copyLen = std::min(len, vfs::DESKTOP_INI.size() - pos);
        if (copyLen > 0) {
            memcpy(dst, vfs::DESKTOP_INI.data() + pos, copyLen);
        }
        result(copyLen, 0);
        return 0;
#endif
    }
    
    if (boost::ends_with(entry.name, ".dng")) {
        return generateFrame(entry, pos, len, dst, result, async);
    }
    
    result(0, -1);
    return -1;
}

size_t VirtualFileSystemImpl_DirectLog::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {

    auto renderTask = [this, entry, pos, len, dst, result]() -> size_t {
        try {
            auto data = materializeFile(entry, false);
            const size_t count = data && pos < data->size()
                ? std::min(len, data->size() - pos) : 0;
            if (count)
                std::memcpy(dst, data->data() + pos, count);
            result(count, 0);
            return count;
        } catch (const std::exception& e) {
            spdlog::error("Error generating DirectLog frame: {}", e.what());
            result(0, -1);
            return 0;
        }
    };
    auto renderFuture = mProcessingThreadPool.submit_task(renderTask);
    return async ? 0 : renderFuture.get();
}

void VirtualFileSystemImpl_DirectLog::loadSidecarMetadata(
        const boost::filesystem::path& path) {
    mSidecarMetadata = nlohmann::json();
    try {
        std::ifstream input(path.string());
        if (input) input >> mSidecarMetadata;
    } catch (const std::exception& e) {
        spdlog::warn("Could not parse DirectLog dynamic metadata from {}: {}",
                     path.string(), e.what());
        mSidecarMetadata = nlohmann::json();
    }
}

std::vector<GainMap> VirtualFileSystemImpl_DirectLog::loadSidecarGainMaps(
        int frameNumber, const char* field) const {
    std::vector<GainMap> maps;
    if (!mSidecarMetadata.contains("dynamic")) return maps;
    const auto& dynamic = mSidecarMetadata["dynamic"];
    if (!dynamic.contains("frames") || !dynamic.contains("gainMapFormats") ||
        !dynamic.contains("gainMapPayloads") || frameNumber < 0 ||
        static_cast<size_t>(frameNumber) >= dynamic["frames"].size()) return maps;
    const auto& frame = dynamic["frames"][frameNumber];
    if (!frame.contains(field) || !frame[field].is_array()) return maps;

    for (const auto& reference : frame[field]) {
        const size_t formatIndex = reference.at("format").get<size_t>();
        const size_t payloadIndex = reference.at("payload").get<size_t>();
        if (formatIndex >= dynamic["gainMapFormats"].size() ||
            payloadIndex >= dynamic["gainMapPayloads"].size())
            throw std::runtime_error("Invalid DirectLog gain-map reference");
        const auto& format = dynamic["gainMapFormats"][formatIndex];
        const auto& payload = dynamic["gainMapPayloads"][payloadIndex];
        const size_t count = payload.at("valueCount").get<size_t>();
        if (count > std::numeric_limits<uint32_t>::max() / sizeof(float))
            throw std::runtime_error("DirectLog gain-map payload is too large");
        const QByteArray compressed = QByteArray::fromBase64(
            QByteArray::fromStdString(payload.at("values").get<std::string>()));
        QByteArray wrapped;
        const uint32_t byteCount = static_cast<uint32_t>(count * sizeof(float));
        wrapped.reserve(compressed.size() + 4);
        wrapped.append(static_cast<char>(byteCount >> 24));
        wrapped.append(static_cast<char>(byteCount >> 16));
        wrapped.append(static_cast<char>(byteCount >> 8));
        wrapped.append(static_cast<char>(byteCount));
        wrapped.append(compressed);
        const QByteArray raw = qUncompress(wrapped);
        if (raw.size() != static_cast<qsizetype>(byteCount))
            throw std::runtime_error("Could not decompress DirectLog gain-map values");

        GainMap map{};
        map.top = format.at("top").get<uint32_t>();
        map.left = format.at("left").get<uint32_t>();
        map.bottom = format.at("bottom").get<uint32_t>();
        map.right = format.at("right").get<uint32_t>();
        map.coordinateWidth = format.at("coordinateWidth").get<uint32_t>();
        map.coordinateHeight = format.at("coordinateHeight").get<uint32_t>();
        map.plane = format.at("plane").get<uint32_t>();
        map.planes = format.at("planes").get<uint32_t>();
        map.rowPitch = format.at("rowPitch").get<uint32_t>();
        map.colPitch = format.at("colPitch").get<uint32_t>();
        map.width = format.at("width").get<uint32_t>();
        map.height = format.at("height").get<uint32_t>();
        map.channels = format.at("channels").get<uint32_t>();
        map.spacingV = format.at("spacingV").get<double>();
        map.spacingH = format.at("spacingH").get<double>();
        map.originV = format.at("originV").get<double>();
        map.originH = format.at("originH").get<double>();
        if (!map.width || !map.height || !map.channels ||
            count != static_cast<size_t>(map.width) * map.height * map.channels)
            throw std::runtime_error("Invalid DirectLog gain-map dimensions");
        map.data.resize(count);
        const auto* source = reinterpret_cast<const unsigned char*>(raw.constData());
        for (size_t index = 0; index < count; ++index) {
            const uint32_t bits = static_cast<uint32_t>(source[index * 4]) |
                (static_cast<uint32_t>(source[index * 4 + 1]) << 8) |
                (static_cast<uint32_t>(source[index * 4 + 2]) << 16) |
                (static_cast<uint32_t>(source[index * 4 + 3]) << 24);
            std::memcpy(&map.data[index], &bits, sizeof(bits));
        }
        maps.push_back(std::move(map));
    }
    return maps;
}

void VirtualFileSystemImpl_DirectLog::applySidecarGainMaps(
        std::vector<uint16_t>& rgbData, int frameNumber, float& exposureOffset,
        std::array<float, 3>& neutralScale) const {
    if (!(mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) ||
        !mSidecarMetadata.contains("dynamic")) return;
    const auto& dynamic = mSidecarMetadata["dynamic"];
    if (!dynamic.contains("frames") || !dynamic.contains("gainMapFormats") ||
        !dynamic.contains("gainMapPayloads") || frameNumber < 0 ||
        static_cast<size_t>(frameNumber) >= dynamic["frames"].size()) return;

    const auto& frame = dynamic["frames"][frameNumber];
    std::string cfaPhase = "bggr";
    if (mCalibration && !mCalibration->cfaPhase.empty())
        cfaPhase = mCalibration->cfaPhase;
    else if (!mConfig.cfaPhase.empty() && mConfig.cfaPhase != "Don't override CFA")
        cfaPhase = mConfig.cfaPhase;
    std::transform(cfaPhase.begin(), cfaPhase.end(), cfaPhase.begin(), ::tolower);
    std::array<uint8_t, 4> cfa{2, 1, 1, 0};
    if (cfaPhase == "rggb") cfa = {0, 1, 1, 2};
    else if (cfaPhase == "grbg") cfa = {1, 0, 2, 1};
    else if (cfaPhase == "gbrg") cfa = {1, 2, 0, 1};
    if (mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP)
        std::fill(rgbData.begin(), rgbData.end(), std::numeric_limits<uint16_t>::max());
    auto apply = [&](const char* field) {
        if (!frame.contains(field) || !frame[field].is_array()) return;
        std::array<float, 3> optimizedMinima{1.0f, 1.0f, 1.0f};
        if (mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) {
            optimizedMinima.fill(std::numeric_limits<float>::max());
            for (const auto& map : loadSidecarGainMaps(frameNumber, field)) {
                std::array<bool, 3> mapColors{};
                if (map.channels == 1) {
                    for (uint32_t phaseY = 0; phaseY < 2; ++phaseY)
                        for (uint32_t phaseX = 0; phaseX < 2; ++phaseX)
                            if (phaseY % map.rowPitch == 0 && phaseX % map.colPitch == 0)
                                mapColors[std::min<size_t>(2, cfa[
                                    (((map.top + phaseY) & 1u) << 1u) |
                                    ((map.left + phaseX) & 1u)])] = true;
                }
                if ((mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) &&
                    map.channels == 1 && mapColors[0] && mapColors[1] && mapColors[2])
                    continue;
                for (size_t point = 0;
                     point < static_cast<size_t>(map.width) * map.height; ++point) {
                    for (uint32_t channel = 0; channel < map.channels; ++channel) {
                        const float value = map.data[point * map.channels + channel];
                        if (!std::isfinite(value) || value <= 0.0f) continue;
                        if (map.channels == 1) {
                            for (size_t color = 0; color < mapColors.size(); ++color)
                                if (mapColors[color])
                                    optimizedMinima[color] = std::min(optimizedMinima[color], value);
                        } else {
                            const size_t color = map.channels >= 4
                                ? std::min<size_t>(2, cfa[channel % 4])
                                : std::min<size_t>(2, channel);
                            optimizedMinima[color] = std::min(optimizedMinima[color], value);
                        }
                    }
                }
            }
            for (float& minimum : optimizedMinima)
                if (!std::isfinite(minimum) || minimum <= 0.0f) minimum = 1.0f;
            const float commonMinimum = *std::min_element(
                optimizedMinima.begin(), optimizedMinima.end());
            exposureOffset += std::log2(commonMinimum);
            for (size_t color = 0; color < optimizedMinima.size(); ++color)
                neutralScale[color] *= commonMinimum / optimizedMinima[color];
        }
        for (const auto& reference : frame[field]) {
            const size_t formatIndex = reference.at("format").get<size_t>();
            const size_t payloadIndex = reference.at("payload").get<size_t>();
            if (formatIndex >= dynamic["gainMapFormats"].size() ||
                payloadIndex >= dynamic["gainMapPayloads"].size())
                throw std::runtime_error("Invalid DirectLog gain-map reference");
            const auto& format = dynamic["gainMapFormats"][formatIndex];
            const auto& payload = dynamic["gainMapPayloads"][payloadIndex];
            const size_t count = payload.at("valueCount").get<size_t>();
            const QByteArray compressed = QByteArray::fromBase64(
                QByteArray::fromStdString(payload.at("values").get<std::string>()));
            QByteArray wrapped;
            wrapped.reserve(compressed.size() + 4);
            const uint32_t byteCount = static_cast<uint32_t>(count * sizeof(float));
            wrapped.append(static_cast<char>(byteCount >> 24));
            wrapped.append(static_cast<char>(byteCount >> 16));
            wrapped.append(static_cast<char>(byteCount >> 8));
            wrapped.append(static_cast<char>(byteCount));
            wrapped.append(compressed);
            const QByteArray raw = qUncompress(wrapped);
            if (raw.size() != static_cast<qsizetype>(byteCount))
                throw std::runtime_error("Could not decompress DirectLog gain-map values");
            std::vector<float> values(count);
            const auto* source = reinterpret_cast<const unsigned char*>(raw.constData());
            for (size_t index = 0; index < count; ++index) {
                const uint32_t bits = static_cast<uint32_t>(source[index * 4]) |
                    (static_cast<uint32_t>(source[index * 4 + 1]) << 8) |
                    (static_cast<uint32_t>(source[index * 4 + 2]) << 16) |
                    (static_cast<uint32_t>(source[index * 4 + 3]) << 24);
                std::memcpy(&values[index], &bits, sizeof(bits));
            }
            const uint32_t mapWidth = format.at("width").get<uint32_t>();
            const uint32_t mapHeight = format.at("height").get<uint32_t>();
            const uint32_t channels = format.at("channels").get<uint32_t>();
            if (!mapWidth || !mapHeight || !channels ||
                values.size() != static_cast<size_t>(mapWidth) * mapHeight * channels)
                throw std::runtime_error("Invalid DirectLog gain-map dimensions");
            const uint32_t top = format.at("top").get<uint32_t>();
            const uint32_t left = format.at("left").get<uint32_t>();
            const uint32_t bottom = format.at("bottom").get<uint32_t>();
            const uint32_t right = format.at("right").get<uint32_t>();
            const uint32_t rowPitch = format.at("rowPitch").get<uint32_t>();
            const uint32_t colPitch = format.at("colPitch").get<uint32_t>();
            const double spacingV = format.at("spacingV").get<double>();
            const double spacingH = format.at("spacingH").get<double>();
            const double originV = format.at("originV").get<double>();
            const double originH = format.at("originH").get<double>();
            if (!format.contains("coordinateWidth") || !format.contains("coordinateHeight"))
                throw std::runtime_error(
                    "DirectLog gain-map sidecar lacks sensor coordinate dimensions");
            const uint32_t coordinateWidth = format.at("coordinateWidth").get<uint32_t>();
            const uint32_t coordinateHeight = format.at("coordinateHeight").get<uint32_t>();
            if (!rowPitch || !colPitch || top >= bottom || left >= right)
                throw std::runtime_error("Invalid DirectLog gain-map geometry");
            if (!coordinateWidth || !coordinateHeight)
                throw std::runtime_error("Invalid DirectLog gain-map coordinate extent");
            // Match the CFA opcode's row/column selection. Pitch-one maps apply
            // to every CFA color (for example deferred luminance); pitch-two
            // maps normally select one or two phases through top/left.
            std::array<bool, 3> affectedColors{};
            for (uint32_t phaseY = 0; phaseY < 2; ++phaseY)
                for (uint32_t phaseX = 0; phaseX < 2; ++phaseX) {
                    const uint32_t sampleY = top + phaseY;
                    const uint32_t sampleX = left + phaseX;
                    if ((sampleY - top) % rowPitch == 0 &&
                        (sampleX - left) % colPitch == 0)
                        affectedColors[std::min<size_t>(2, cfa[
                            ((sampleY & 1u) << 1u) | (sampleX & 1u)])] = true;
                }
            // A single-channel map is the deferred luminance remainder from
            // an earlier color-only bake. Reduce-to-color must leave it
            // deferred; it contains no channel-relative correction to apply.
            if ((mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) && channels == 1 &&
                affectedColors[0] && affectedColors[1] && affectedColors[2])
                continue;

            if ((mConfig.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) && !values.empty()) {
                for (size_t point = 0; point < static_cast<size_t>(mapWidth) * mapHeight; ++point)
                    for (uint32_t channel = 0; channel < channels; ++channel) {
                        size_t color = std::min<size_t>(2, channel);
                        if (channels >= 4) color = std::min<size_t>(2, cfa[channel % 4]);
                        else if (channels == 1) {
                            color = 0;
                            while (color + 1 < affectedColors.size() && !affectedColors[color])
                                ++color;
                        }
                        values[point * channels + channel] /= optimizedMinima[color];
                    }
            }
            if ((mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) && channels > 1) {
                for (size_t point = 0; point < static_cast<size_t>(mapWidth) * mapHeight; ++point) {
                    float minimum = std::numeric_limits<float>::max();
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        minimum = std::min(minimum, values[point * channels + channel]);
                    if (std::isfinite(minimum) && minimum > 0.0f)
                        for (uint32_t channel = 0; channel < channels; ++channel)
                            values[point * channels + channel] /= minimum;
                }
            }
            if (mConfig.options & RENDER_OPT_NORMALIZE_SHADING_MAP) {
                const float maximum = *std::max_element(values.begin(), values.end());
                if (std::isfinite(maximum) && maximum > 0.0f)
                    for (float& value : values) value /= maximum;
            } else if (mConfig.options & RENDER_OPT_DEBUG_SHADING_MAP) {
                for (float& value : values)
                    if (std::isfinite(value) && value > 0.0f) value = 1.0f / value;
            }
            auto gainAt = [&](uint32_t x, uint32_t y, uint32_t channel) {
                channel = std::min(channel, channels - 1);
                return values[(static_cast<size_t>(y) * mapWidth + x) * channels + channel];
            };
            auto colorGainAt = [&](uint32_t x, uint32_t y, uint32_t color) {
                if (channels == 1) return gainAt(x, y, 0);
                float sum = 0.0f;
                uint32_t countForColor = 0;
                for (uint32_t channel = 0; channel < std::min<uint32_t>(4, channels); ++channel) {
                    if (cfa[channel] == color) {
                        sum += gainAt(x, y, channel);
                        ++countForColor;
                    }
                }
                return countForColor ? sum / countForColor : 1.0f;
            };
            for (int y = 0; y < mHeight; ++y) for (int x = 0; x < mWidth; ++x) {
                const uint32_t sensorX = left + static_cast<uint32_t>(x);
                const uint32_t sensorY = top + static_cast<uint32_t>(y);
                const double nx = static_cast<double>(sensorX) / coordinateWidth;
                const double ny = static_cast<double>(sensorY) / coordinateHeight;
                const double gridX = spacingH > 0.0 ? (nx - originH) / spacingH : 0.0;
                const double gridY = spacingV > 0.0 ? (ny - originV) / spacingV : 0.0;
                const double floorX = std::floor(gridX), floorY = std::floor(gridY);
                const uint32_t x0 = static_cast<uint32_t>(std::min<double>(
                    mapWidth - 1, std::max(0.0, floorX)));
                const uint32_t y0 = static_cast<uint32_t>(std::min<double>(
                    mapHeight - 1, std::max(0.0, floorY)));
                const uint32_t x1 = std::min(x0 + 1, mapWidth - 1);
                const uint32_t y1 = std::min(y0 + 1, mapHeight - 1);
                const float fx = static_cast<float>(std::clamp(gridX - floorX, 0.0, 1.0));
                const float fy = static_cast<float>(std::clamp(gridY - floorY, 0.0, 1.0));
                const size_t pixel = (static_cast<size_t>(y) * mWidth + x) * 3;
                for (uint32_t color = 0; color < 3; ++color) {
                    if (channels == 1 && !affectedColors[color]) continue;
                    const float top = colorGainAt(x0, y0, color) * (1.0f - fx) +
                                      colorGainAt(x1, y0, color) * fx;
                    const float bottom = colorGainAt(x0, y1, color) * (1.0f - fx) +
                                         colorGainAt(x1, y1, color) * fx;
                    const float gain = top * (1.0f - fy) + bottom * fy;
                    rgbData[pixel + color] = static_cast<uint16_t>(std::clamp(
                        std::lround(rgbData[pixel + color] * gain), 0l, 65535l));
                }
            }
        }
    };
    apply("gainMaps");
    if (!(mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR))
        apply("deferredGainMaps");
}

bool VirtualFileSystemImpl_DirectLog::convertRGBToDNG(
    const std::vector<uint16_t>& rgbData, 
    std::vector<uint8_t>& dngData, 
    int frameNumber, 
    Timestamp timestamp,
    bool jpegCompression, float gainMapExposureOffset,
    const std::array<float, 3>& gainMapNeutralScale, double iso,
    double shutterSpeed, double baselineExposure,
    const std::optional<std::array<float, 3>>& asShotNeutral,
    const std::vector<GainMap>& opcodeList2Maps,
    const std::vector<GainMap>& opcodeList3Maps) {
    
    try {
        const auto& videoInfo = mDecoder->getVideoInfo();
        int width = videoInfo.width;
        int height = videoInfo.height;
        
        // Determine if we should apply log curve and bit reduction
        const bool applyLogCurve =
            (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
            mConfig.logTransform != LogTransformMode::Disabled;
        int bitReduction = 0;
        
        if (applyLogCurve) {
            // Parse bit reduction from logTransform option
            if (mConfig.logTransform == LogTransformMode::ReduceBy2Bit) {
                bitReduction = 2;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy4Bit) {
                bitReduction = 4;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy6Bit) {
                bitReduction = 6;
            } else if (mConfig.logTransform == LogTransformMode::ReduceBy8Bit) {
                bitReduction = 8;
            } else if (mConfig.logTransform == LogTransformMode::KeepInput) {
                bitReduction = 0;
            }
        }
        
        // Process RGB data: apply log curve to reduce to 12-bit, then apply additional bit reduction
        std::vector<uint16_t> processedRgbData = rgbData;
        float dstWhiteLevel = 65535.0f;
        int encodeBits = 16;

        // Spatial reduction must happen while the decoded RGB values are
        // still linear. Averaging LOG60 values would darken mixed blocks.
        const int proxyScale = vfs::getScaleFromOptions(mConfig.options, mConfig.draftScale);
        if (proxyScale > 1) {
            std::vector<uint16_t> reduced;
            uint32_t reducedWidth = 0, reducedHeight = 0;
            utils::reduceRGB(processedRgbData, reduced,
                             static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                             static_cast<uint32_t>(proxyScale),
                             mConfig.options & RENDER_OPT_HIGHER_CFA_HQ,
                             reducedWidth, reducedHeight);
            if (reduced.empty())
                throw std::runtime_error("Proxy scale is too large for the DirectLog image");
            processedRgbData = std::move(reduced);
            width = static_cast<int>(reducedWidth);
            height = static_cast<int>(reducedHeight);
        }
        
        if (applyLogCurve) {
            // First reduce to 12-bit using log curve
            int useBits = 12;
            dstWhiteLevel = std::pow(2.0f, useBits) - 1.0f; // 4095 for 12-bit
            
            // Apply additional bit reduction if specified
            if (bitReduction > 0) {
                useBits = std::max(1, useBits - bitReduction);
                dstWhiteLevel = std::pow(2.0f, useBits) - 1.0f;
            }
            
            encodeBits = useBits;
            const auto linearRgbData = std::move(processedRgbData);
            processedRgbData.resize(linearRgbData.size());
            
            // Apply log curve to each pixel
            for (size_t i = 0; i < linearRgbData.size(); i += 3) {
                for (int c = 0; c < 3; ++c) {
                    // Normalize input to [0, 1]
                    float normalized = linearRgbData[i + c] / 65535.0f;
                    
                    // Apply log2 transform: log2(1 + k*x) / log2(1 + k)
                    // Using k=60 to match MCRAW implementation
                    float logValue = std::log2(1.0f + 60.0f * normalized) / std::log2(61.0f);
                    
                    // Scale to target bit depth
                    float scaled = logValue * dstWhiteLevel;
                    
                    // Clamp and round
                    processedRgbData[i + c] = static_cast<uint16_t>(
                        std::clamp(std::round(scaled), 0.0f, dstWhiteLevel)
                    );
                }
            }
        }
        
        // Check if remosaicing is requested (from render options)
        bool shouldRemosaic = (mConfig.options & RENDER_OPT_REMOSAIC_TO_BAYER) != 0;
        
        // Get CFA phase from calibration JSON if available, otherwise use UI setting
        std::string cfaPhase = "bggr";
        if (mCalibration.has_value() && !mCalibration->cfaPhase.empty()) {
            // JSON override takes priority
            cfaPhase = mCalibration->cfaPhase;
        } else if (!mConfig.cfaPhase.empty() && mConfig.cfaPhase != "Don't override CFA") {
            // Use UI setting if not "Don't override CFA"
            cfaPhase = mConfig.cfaPhase;
        }
        // else: keep default "bggr"
        
        // Convert to lowercase for consistency
        std::transform(cfaPhase.begin(), cfaPhase.end(), cfaPhase.begin(), ::tolower);
        
        std::vector<uint16_t> imageSamples;
        int samplesPerPixel = 3;
        int photometric = tinydngwriter::PHOTOMETRIC_LINEARRAW;
        
        if (shouldRemosaic) {
            // Convert RGB to Bayer CFA pattern
            std::vector<uint16_t> bayerData;
            utils::remosaicRGBToBayer(processedRgbData, bayerData, width, height, cfaPhase);
            
            // Pack Bayer data to actual bit depth
            imageSamples = std::move(bayerData);
            
            samplesPerPixel = 1; // Single channel for CFA
            photometric = 32803; // CFA (Color Filter Array)
        } else {
            // Pack RGB data to actual bit depth
            imageSamples = std::move(processedRgbData);
        }

        const bool jpegXlCompression = jpegCompression && mConfig.jxlDistance >= 0.0f;
        std::vector<uint8_t> imageBytes;
        if (jpegCompression || encodeBits == 16) {
            imageBytes.resize(imageSamples.size() * sizeof(uint16_t));
            std::memcpy(imageBytes.data(), imageSamples.data(), imageBytes.size());
        } else {
            imageBytes = packSamples(
                imageSamples,
                static_cast<size_t>(width) * samplesPerPixel,
                static_cast<unsigned int>(encodeBits));
        }
        
        // Create DNG image
        tinydngwriter::DNGImage dng;
        
        // Set basic image properties
        dng.SetBigEndian(false);
        dng.SetImageWidth(width);
        dng.SetImageLength(height);
        dng.SetSamplesPerPixel(samplesPerPixel);
        dng.SetRowsPerStrip(height);
        
        unsigned short bitsPerSample[3] = {
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits),
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits),
            static_cast<unsigned short>(jpegXlCompression ? 16 : encodeBits)
        };
        dng.SetBitsPerSample(samplesPerPixel, bitsPerSample);
        
        // Photometric interpretation
        dng.SetPhotometric(photometric);
        dng.SetPlanarConfig(1); // Chunky
        dng.SetCompression(jpegCompression
            ? (mConfig.jxlDistance < 0.0f ? tinydngwriter::COMPRESSION_JPEG
                                         : tinydngwriter::COMPRESSION_JPEG_XL)
            : tinydngwriter::COMPRESSION_NONE);
        if (jpegCompression && mConfig.jxlDistance >= 0.0f)
            dng.SetJXLDistance(mConfig.jxlDistance);
        
        unsigned short sampleFormat[3] = {1, 1, 1}; // Unsigned integer
        dng.SetSampleFormat(samplesPerPixel, sampleFormat);
        
        // Set CFA pattern if remosaicing
        if (shouldRemosaic) {
            unsigned char cfaPattern[4];
            if (cfaPhase == "bggr") {
                cfaPattern[0] = 2; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 0; // B G G R
            } else if (cfaPhase == "rggb") {
                cfaPattern[0] = 0; cfaPattern[1] = 1; cfaPattern[2] = 1; cfaPattern[3] = 2; // R G G B
            } else if (cfaPhase == "grbg") {
                cfaPattern[0] = 1; cfaPattern[1] = 0; cfaPattern[2] = 2; cfaPattern[3] = 1; // G R B G
            } else { // gbrg
                cfaPattern[0] = 1; cfaPattern[1] = 2; cfaPattern[2] = 0; cfaPattern[3] = 1; // G B R G
            }
            dng.SetCFARepeatPatternDim(2, 2);
            dng.SetCFAPattern(4, cfaPattern);
            dng.SetCFALayout(1); // Rectangular (or square) layout
            dng.SetBlackLevelRepeatDim(2, 2);
            const unsigned int activeArea[4] = {
                0, 0, static_cast<unsigned int>(height),
                static_cast<unsigned int>(width)
            };
            dng.SetActiveArea(activeArea);
        }
        
        // Set DNG version
        dng.SetDNGVersion(1, jpegXlCompression ? 7 : 4, 0, 0);
        if (jpegXlCompression) {
            dng.SetDNGBackwardVersion(1, 7, 0, 0);
        } else if (shouldRemosaic) {
            dng.SetDNGBackwardVersion(1, 1, 0, 0);
        } else {
            dng.SetDNGBackwardVersion(1, 4, 0, 0);
        }
        
        // Set camera/software metadata
        if (mConfig.cameraModel == "Blackmagic") {
            dng.SetUniqueCameraModel("Blackmagic Pocket Cinema Camera 4K");
        } else if (mConfig.cameraModel == "Panasonic") {
            dng.SetUniqueCameraModel("Panasonic Varicam RAW");
        } else if (mConfig.cameraModel == "Fujifilm" ||
                   mConfig.cameraModel == "Fujifilm X-T5") {
            dng.SetUniqueCameraModel("Fujifilm X-T5");
            dng.SetMake("Fujifilm");
            dng.SetCameraModelName("X-T5");
        } else if (!mConfig.cameraModel.empty()) {
            dng.SetUniqueCameraModel(mConfig.cameraModel);
        } else {
            dng.SetUniqueCameraModel("DirectLog Video");
        }
        dng.SetSoftware("MotionCam DirectLog Decoder");
        if (iso > 0.0) dng.SetIso(static_cast<unsigned int>(std::lround(iso)));
        if (shutterSpeed > 0.0) dng.SetExposureTime(shutterSpeed);
        
        // Set image description with frame info
        std::ostringstream desc;
        desc << "Frame " << frameNumber << " from DirectLog video";
        if (mIsHLG) {
            desc << " (HLG to Linear)";
        }
        if (applyLogCurve) {
            desc << " (Log " << encodeBits << "-bit)";
        }
        if (shouldRemosaic) {
            desc << " (Remosaiced " << cfaPhase << ")";
        }
        dng.SetImageDescription(desc.str());
        
        // Set resolution
        dng.SetXResolution(72.0f);
        dng.SetYResolution(72.0f);
        dng.SetResolutionUnit(2); // inches
        if (mFps > 0.0f) {
            dng.SetFrameRate(mFps);
        }

        // Set baseline exposure with the optional gain offset
        float exposureOffset = (mConfig.cameraModel == "Panasonic" ? -2.0f : 0.0f);
        if (!mConfig.exposureCompensation.empty()) {
            try {
                exposureOffset += std::stof(mConfig.exposureCompensation);
            } catch (const std::exception&) {
                // If parsing fails, keep the original exposureOffset value
            }
        }
        dng.SetBaselineExposure(static_cast<float>(baselineExposure) +
                                exposureOffset + gainMapExposureOffset);
        
        // Set white/black levels and linearization table
        if (applyLogCurve) {
            // Create linearization table to reverse the log curve
            const int tableSize = static_cast<int>(dstWhiteLevel) + 1;
            std::vector<unsigned short> linearizationTable(tableSize);
            
            for (int i = 0; i < tableSize; i++) {
                float normalizedLogValue = static_cast<float>(i) / dstWhiteLevel;
                
                float linearValue;
                if (i == 0) {
                    linearValue = 0.0f;  // Exact identity: stored 0 → linear 0
                } else if (i == tableSize - 1) {
                    linearValue = 1.0f;  // Force maximum table entry → linear 1
                } else {
                    // Inverse of: logValue = log2(1 + k*x) / log2(1 + k)
                    // x = (2^(logValue * log2(1 + k)) - 1) / k
                    linearValue = (std::pow(2.0f, normalizedLogValue * std::log2(61.0f)) - 1.0f) / 60.0f;
                    linearValue = std::clamp(linearValue, 0.0f, 1.0f);
                }
                
                // Scale to 16-bit range
                linearizationTable[i] = static_cast<unsigned short>(linearValue * 65535.0f);
            }
            
            dng.SetLinearizationTable(tableSize, linearizationTable.data());
            
            // Set black level to 0 and white level to 65534 (as per MCRAW implementation)
            unsigned short blackLevel[4] = {0, 0, 0, 0};
            dng.SetBlackLevel(shouldRemosaic ? 4 : 3, blackLevel);
            dng.SetWhiteLevel(65534);
        } else {
            // Without a linearization table, white level is the maximum value
            // representable by the stored sample bit depth.
            const unsigned int whiteLevel =
                (static_cast<unsigned int>(1) << encodeBits) - 1;
            dng.SetWhiteLevel(whiteLevel);
            unsigned short blackLevel[4] = {0, 0, 0, 0};
            dng.SetBlackLevel(shouldRemosaic ? 4 : 3, blackLevel);
        }
        
        if (!dng.SetImageData(imageBytes.data(), imageBytes.size())) {
            throw std::runtime_error("Failed to attach DirectLog image data");
        }
        
        // Apply calibration if available
        if (mCalibration.has_value()) {
            if (mCalibration->hasColorMatrix1 || mCalibration->hasForwardMatrix1) {
                dng.SetCalibrationIlluminant1(21); // D65
            }
            if (mCalibration->hasColorMatrix2 || mCalibration->hasForwardMatrix2) {
                dng.SetCalibrationIlluminant2(17); // Standard Light A
            }
            if (mCalibration->hasColorMatrix1) {
                dng.SetColorMatrix1(3, mCalibration->colorMatrix1.data());
            }
            if (mCalibration->hasColorMatrix2) {
                dng.SetColorMatrix2(3, mCalibration->colorMatrix2.data());
            }
            if (mCalibration->hasForwardMatrix1) {
                dng.SetForwardMatrix1(3, mCalibration->forwardMatrix1.data());
            }
            if (mCalibration->hasForwardMatrix2) {
                dng.SetForwardMatrix2(3, mCalibration->forwardMatrix2.data());
            }
        }
        auto outputNeutral = asShotNeutral;
        if (!outputNeutral && mCalibration && mCalibration->hasAsShotNeutral)
            outputNeutral = mCalibration->asShotNeutral;
        if (outputNeutral) {
            for (size_t color = 0; color < outputNeutral->size(); ++color)
                (*outputNeutral)[color] *= gainMapNeutralScale[color];
            dng.SetAsShotNeutral(3, outputNeutral->data());
        }
        auto makeOpcodeList = [&](const std::vector<GainMap>& maps) {
            tinydngwriter::OpcodeList result;
            if (maps.empty()) return result;
            const uint32_t cropLeft = std::min_element(maps.begin(), maps.end(),
                [](const GainMap& a, const GainMap& b) { return a.left < b.left; })->left;
            const uint32_t cropTop = std::min_element(maps.begin(), maps.end(),
                [](const GainMap& a, const GainMap& b) { return a.top < b.top; })->top;
            for (const auto& map : maps) {
                if (!map.coordinateWidth || !map.coordinateHeight ||
                    map.left < cropLeft || map.top < cropTop ||
                    map.right <= cropLeft || map.bottom <= cropTop)
                    throw std::runtime_error("Invalid DirectLog opcode coordinate geometry");
                tinydngwriter::GainMapParams params{};
                params.top = map.top - cropTop; params.left = map.left - cropLeft;
                params.bottom = std::min<uint32_t>(mHeight, map.bottom - cropTop);
                params.right = std::min<uint32_t>(mWidth, map.right - cropLeft);
                params.plane = map.plane; params.planes = map.planes;
                params.row_pitch = map.rowPitch; params.col_pitch = map.colPitch;
                params.map_points_v = map.height; params.map_points_h = map.width;
                params.map_spacing_v = map.spacingV * map.coordinateHeight / mHeight;
                params.map_spacing_h = map.spacingH * map.coordinateWidth / mWidth;
                params.map_origin_v =
                    (map.originV * map.coordinateHeight - cropTop) / mHeight;
                params.map_origin_h =
                    (map.originH * map.coordinateWidth - cropLeft) / mWidth;
                params.map_planes = map.channels;
                // The sidecar and DNG payload are point-major/interleaved;
                // tiny_dng_writer accepts plane-major input.
                const size_t planeSize = static_cast<size_t>(map.width) * map.height;
                params.gain_data.resize(map.data.size());
                for (size_t point = 0; point < planeSize; ++point)
                    for (uint32_t channel = 0; channel < map.channels; ++channel)
                        params.gain_data[static_cast<size_t>(channel) * planeSize + point] =
                            map.data[point * map.channels + channel];
                result.AddGainMap(params);
            }
            return result;
        };
        const auto opcodeList2 = makeOpcodeList(opcodeList2Maps);
        const auto opcodeList3 = makeOpcodeList(opcodeList3Maps);
        if (!opcodeList2.IsEmpty()) dng.SetOpcodeList2(opcodeList2);
        if (!opcodeList3.IsEmpty()) dng.SetOpcodeList3(opcodeList3);
        // Write DNG to memory stream
        std::ostringstream oss;
        tinydngwriter::DNGWriter writer(false); // little-endian
        writer.AddImage(&dng);
        
        std::string err;
        if (!writer.WriteToFile(oss, &err)) {
            spdlog::error("Failed to write DNG for frame {}: {}", frameNumber, err);
            return false;
        }
        
        // Copy to output vector
        std::string dngStr = oss.str();
        dngData.assign(dngStr.begin(), dngStr.end());
        
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception in convertRGBToDNG for frame {}: {}", frameNumber, e.what());
        return false;
    }
}

std::shared_ptr<std::vector<char>> VirtualFileSystemImpl_DirectLog::materializeFile(
    const Entry& entry, bool jpegCompression) {
    if (!jpegCompression) {
        if (auto cached = mCache.get(entry)) {
            mCache.put(entry, cached);
            return cached;
        }
    }

    try {
        const auto timestamp = std::get<Timestamp>(entry.userData);
        const auto& frames = mDecoder->getFrames();
        const auto it = std::find_if(frames.begin(), frames.end(), [timestamp](const auto& frame) {
            return frame.timestamp == timestamp;
        });
        if (it == frames.end())
            throw std::runtime_error("DirectLog source frame not found");
        const int frameNumber = static_cast<int>(std::distance(frames.begin(), it));

        std::vector<uint16_t> rgbData;
        if (!mDecoder->extractFrame(frameNumber, rgbData))
            throw std::runtime_error("Could not decode DirectLog frame");
        const auto sidecarGainMaps = loadSidecarGainMaps(frameNumber, "gainMaps");
        const auto sidecarDeferredGainMaps = loadSidecarGainMaps(frameNumber, "deferredGainMaps");
        float gainMapExposureOffset = 0.0f;
        std::array<float, 3> gainMapNeutralScale{1.0f, 1.0f, 1.0f};
        applySidecarGainMaps(rgbData, frameNumber, gainMapExposureOffset, gainMapNeutralScale);
        double iso = 0.0, shutterSpeed = 0.0, baselineExposure = 0.0;
        std::optional<std::array<float, 3>> asShotNeutral;
        if (mSidecarMetadata.contains("dynamic") &&
            mSidecarMetadata["dynamic"].contains("frames") &&
            static_cast<size_t>(frameNumber) < mSidecarMetadata["dynamic"]["frames"].size()) {
            const auto& metadata = mSidecarMetadata["dynamic"]["frames"][frameNumber];
            iso = metadata.value("iso", 0.0);
            shutterSpeed = metadata.value("shutterSpeedSeconds", 0.0);
            baselineExposure = metadata.value("baselineExposure", 0.0);
            if (metadata.contains("asShotNeutral") &&
                metadata["asShotNeutral"].is_array() &&
                metadata["asShotNeutral"].size() >= 3) {
                asShotNeutral = std::array<float, 3>{
                    metadata["asShotNeutral"][0].get<float>(),
                    metadata["asShotNeutral"][1].get<float>(),
                    metadata["asShotNeutral"][2].get<float>()};
            }
        }
        const auto frameDigits = entry.name.substr(entry.name.size() - 10, 6);
        const int outputFrameNumber = std::stoi(frameDigits);
        std::vector<GainMap> opcodeList2Maps;
        std::vector<GainMap> opcodeList3Maps;
        auto classifyGainMaps = [&](const std::vector<GainMap>& maps) {
            if (maps.empty()) return;
            const bool cfaLensSet = maps.size() == 4 ||
                (maps.size() == 1 && maps.front().channels == 4);
            const bool deferredLuminance = maps.size() == 1 &&
                maps.front().channels == 1;
            if (cfaLensSet)
                opcodeList2Maps.insert(opcodeList2Maps.end(), maps.begin(), maps.end());
            else if (deferredLuminance)
                opcodeList3Maps.push_back(maps.front());
            else
                throw std::runtime_error("Unsupported DirectLog gain-map layout");
        };
        if (!(mConfig.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION)) {
            classifyGainMaps(sidecarGainMaps);
            classifyGainMaps(sidecarDeferredGainMaps);
        } else if (mConfig.options & RENDER_OPT_VIGNETTE_ONLY_COLOR) {
            if (sidecarDeferredGainMaps.size() == 1 &&
                sidecarDeferredGainMaps.front().channels == 1)
                opcodeList3Maps = sidecarDeferredGainMaps;
            else if (!sidecarDeferredGainMaps.empty())
                throw std::runtime_error("Unsupported DirectLog deferred gain-map layout");
            if (sidecarGainMaps.size() == 1 && sidecarGainMaps.front().channels == 1)
                opcodeList3Maps = sidecarGainMaps;
        }
        std::vector<uint8_t> dngData;
        if (!convertRGBToDNG(rgbData, dngData, outputFrameNumber, timestamp, jpegCompression,
                             gainMapExposureOffset, gainMapNeutralScale, iso, shutterSpeed,
                             baselineExposure, asShotNeutral,
                             opcodeList2Maps, opcodeList3Maps))
            throw std::runtime_error("Could not generate DirectLog DNG");
        const bool converted = mConfig.options & RENDER_OPT_FRAMERATE_CONVERSION;
        const Timestamp outputTimestamp = converted
            ? static_cast<Timestamp>(std::llround(outputFrameNumber * 1e9 / mFps))
            : timestamp - frames.front().timestamp;
        if (!DNGDecoder::setTimingMetadata(dngData, mFps, outputTimestamp))
            throw std::runtime_error("Could not write DirectLog DNG timing metadata");

        auto output = std::make_shared<std::vector<char>>(dngData.begin(), dngData.end());
        if (!jpegCompression)
            mCache.put(entry, output);
        return output;
    } catch (...) {
        if (!jpegCompression)
            mCache.markLoadFailed(entry);
        throw;
    }
}

void VirtualFileSystemImpl_DirectLog::updateOptions(const RenderSettings& config) {
    std::lock_guard<std::mutex> lock(mMutex);

    mCache.clear();
    mConfig = config;
    
    // Reload calibration JSON if it exists
    boost::filesystem::path srcPath(mSrcPath);
    boost::filesystem::path calibPath = srcPath.parent_path() / (srcPath.stem().string() + ".json");
    mCalibration.reset();
    mSidecarMetadata = nlohmann::json();
    if (boost::filesystem::exists(calibPath)) {
        loadSidecarMetadata(calibPath);
        mCalibration = CalibrationData::loadFromFile(calibPath.string());
        if (mCalibration.has_value()) {
            spdlog::info("Reloaded calibration for DirectLog: {}", calibPath.string());
        }
    }
    mDecoder->setFullRangeOverride(std::nullopt);
    if (mCalibration && mCalibration->hasDataLevels) {
        if (mCalibration->dataLevels == "Full")
            mDecoder->setFullRangeOverride(true);
        else if (mCalibration->dataLevels == "Limited")
            mDecoder->setFullRangeOverride(false);
    }
    
    init();
}

FileInfo VirtualFileSystemImpl_DirectLog::getFileInfo() const {
    FileInfo info;
    info.frameRateInfo = mFrameRateInfo;
    info.fps = mFps;
    info.totalFrames = mTotalFrames;
    info.droppedFrames = mDroppedFrames;
    info.duplicatedFrames = mDuplicatedFrames;
    info.width = mWidth;
    info.height = mHeight;
    
    // This describes the input, not the selected DNG render operation. A
    // companion JSON may explicitly reinterpret the input CFA size.
    const int cfaSize = mCalibration && mCalibration->hasCfaSize && mCalibration->cfaSize > 0
        ? mCalibration->cfaSize : 0;
    info.dataType = vfs::getDisplayDataType(true, cfaSize);
    
    // Determine levels info
    const bool applyLogCurve =
        (mConfig.options & RENDER_OPT_LOG_TRANSFORM) &&
        mConfig.logTransform != LogTransformMode::Disabled;
    int srcBits = 16;
    int dstBits = 16;
    
    if (applyLogCurve) {
        // Strip " lq" suffix if present for comparison
        dstBits = 12; // Start with 12-bit log
        if (mConfig.logTransform == LogTransformMode::ReduceBy2Bit) {
            dstBits = 10;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy4Bit) {
            dstBits = 8;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy6Bit) {
            dstBits = 6;
        } else if (mConfig.logTransform == LogTransformMode::ReduceBy8Bit) {
            dstBits = 4;
        }
    }
    
    int srcWhiteLevel = (1 << srcBits) - 1;
    int dstWhiteLevel = (1 << dstBits) - 1;
    
    info.levelsInfo = std::to_string(srcWhiteLevel) + "/0 -> " + 
                      std::to_string(dstWhiteLevel) + "/0 RAW" + std::to_string(dstBits) +
                      (applyLogCurve ? " log" : "");
    
    // Calculate runtime from video duration
    info.runtimeSeconds = (mFps > 0) ? (static_cast<float>(mTotalFrames) / mFps) : 0.0f;
    
    return info;
}

bool VirtualFileSystemImpl_DirectLog::isHLGVideo() const {
    return mIsHLG;
}

void VirtualFileSystemImpl_DirectLog::calculateFrameRateStats() {
    const auto& frames = mDecoder->getFrames();
    
    if (frames.size() < 2) {
        return;
    }
    
    // Convert frame timestamps to vector of Timestamp
    std::vector<Timestamp> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames) {
        timestamps.push_back(frame.timestamp);
    }
    
    mFrameRateInfo = vfs::calculateFrameRate(timestamps);
    
    spdlog::debug("DirectLog frame rate stats: avg={:.2f}fps, median={:.2f}fps", mFrameRateInfo.averageFrameRate, mFrameRateInfo.medianFrameRate);
}

} // namespace motioncam

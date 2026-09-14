#include "VirtualFileSystemImpl.h"
#include "DataLevels.h"
#include "DNGDecoder.h"
#include "LRUCache.h"
#include "CalibrationData.h"
#include "GainMapBake.h"
#include "Utils.h"
#include <motioncam/Decoder.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <sstream>
#include <iomanip>
#include <array>
#include <fstream>
#include <filesystem>
#include <QProcess>
#include <QTemporaryDir>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <spdlog/spdlog.h>
#include <QByteArray>
#include <nlohmann/json.hpp>
#include <tinydng/tiny_dng_writer.h>
#include <cstring>
#include <BS_thread_pool.hpp>

namespace motioncam {
namespace vfs {

DngRenderPlan planDngRender(
        const Entry& entry, Timestamp sourceTimestamp,
        Timestamp firstSourceTimestamp, const RenderSettings& settings,
        float frameRate, bool finalizing, bool numberedSequence) {
    DngRenderPlan plan;
    plan.outputFrameNumber = outputFrameNumber(entry);
    plan.scale = getScaleFromOptions(settings.options, settings.draftScale);
    plan.nativeMetadataFrame = !finalizing && !settings.streamingPreview &&
        numberedSequence && plan.outputFrameNumber == 0 && plan.scale > 1;
    if (plan.nativeMetadataFrame) plan.scale = 1;
    plan.outputTimestamp = outputTimestamp(
        entry, sourceTimestamp, firstSourceTimestamp, frameRate,
        settings.options & RENDER_OPT_FRAMERATE_CONVERSION);
    return plan;
}

void processDngPixels(std::vector<uint8_t>& dng,
        const RenderSettings& settings,
        const DngPixelPipelineOptions& options) {
    const std::string source = options.sourceName.empty()
        ? std::string("frame") : std::string(options.sourceName);
    // Level correction is tag-only for every source. It deliberately precedes
    // any operation that consults the DNG's black/white metadata and never
    // requantizes the stored samples.
    if (!DNGDecoder::overrideDataLevels(dng, settings.levels))
        throw std::runtime_error("Could not override data levels for " + source);
    const auto demosaics = [](QuadBayerMode value) {
        return value == QuadBayerMode::Demosaic ||
            value == QuadBayerMode::DemosaicColor ||
            value == QuadBayerMode::DemosaicOCL;
    };
    if (options.hasCfa && options.calibration &&
        options.calibration->hasBadPixels &&
        settings.badPixelTreatment != BadPixelTreatment::Disabled) {
        DecodedDNGImage decoded;
        if (!DNGDecoder::decodeImage(dng, decoded, false, false) ||
            decoded.layout.pixels != DNGPixelLayout::CFA)
            throw std::runtime_error("Could not decode CFA DNG for bad-pixel treatment: " +
                                     source);
        const float white = decoded.metadata.whiteLevelCount
            ? decoded.metadata.whiteLevel[0] : 65535.0f;
        const bool willDemosaic = demosaics(settings.quadBayerOption) &&
            (options.cfaRepeatSize > 2 || settings.cameraNativeStaging ||
             options.outputScale > 1);
        const int originalWidth = options.calibration->hasFullSensorResolution
            ? options.calibration->fullSensorResolution[0]
            : static_cast<int>(decoded.layout.width);
        const int originalHeight = options.calibration->hasFullSensorResolution
            ? options.calibration->fullSensorResolution[1]
            : static_cast<int>(decoded.layout.height);
        const auto active = utils::applyCfaBadPixels(
            decoded.samples.data(), decoded.layout.width, decoded.layout.height,
            originalWidth, originalHeight, options.cfaRepeatSize, white,
            decoded.metadata.blackLevel, options.iso, options.exposureTime,
            *options.calibration, settings.badPixelTreatment, willDemosaic);
        if (settings.badPixelTreatment == BadPixelTreatment::Bake || willDemosaic) {
            if (!DNGDecoder::encodeImage(dng, decoded))
                throw std::runtime_error("Could not encode corrected CFA DNG: " + source);
        } else if (!active.empty()) {
            tinydngwriter::FixBadPixelsParams params;
            for (const auto& pixel : active)
                params.bad_pixels.push_back({pixel.row, pixel.column});
            std::string phaseName;
            for (const uint8_t color : options.cfaPhase)
                phaseName += color == 0 ? 'r' : color == 2 ? 'b' : 'g';
            params.bayer_phase = phaseName == "rggb" ? 0
                : phaseName == "grbg" ? 1 : phaseName == "gbrg" ? 2 : 3;
            tinydngwriter::OpcodeList opcodes;
            opcodes.AddFixBadPixelsList(params);
            const auto payload = opcodes.Serialize();
            if (!DNGDecoder::replaceOpcodeList(dng, 1,
                    std::vector<uint8_t>(payload.begin(), payload.end())))
                throw std::runtime_error("Could not write DNG bad-pixel opcodes: " + source);
        }
    }
    if (settings.options & RENDER_OPT_CROPPING) {
        uint32_t width = 0, height = 0, stride = 0;
        utils::parseCropTarget(settings.cropTarget, width, height, stride);
        if (width && height && !DNGDecoder::cropImage(dng, width, height))
            throw std::runtime_error("Unsupported crop for " + source);
    }
    std::vector<GainMap> opcode2;
    const bool hasOpcode2 = DNGDecoder::getGainMaps(dng, 2, opcode2) &&
        !opcode2.empty();
    std::vector<GainMap> opcode3;
    const bool hasOpcode3Luma = DNGDecoder::hasOnlySinglePlaneGainMap(dng, 3) &&
        DNGDecoder::getGainMaps(dng, 3, opcode3) && opcode3.size() == 1 &&
        opcode3.front().channels == 1;
    const bool colorOnly = settings.options & RENDER_OPT_VIGNETTE_ONLY_COLOR;
    const bool debugGainMap = settings.options & RENDER_OPT_DEBUG_SHADING_MAP;
    const bool bakeGain = (settings.options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) &&
        (hasOpcode2 || (hasOpcode3Luma && !colorOnly) || debugGainMap);
    QuadBayerMode mode = settings.quadBayerOption;
    if (settings.streamingPreview && options.outputScale == 1 &&
        !(settings.options & RENDER_OPT_HIGHER_CFA_HQ) && !debugGainMap &&
        demosaics(mode))
        mode = QuadBayerMode::CorrectQBCFAMetadata;
    const bool remosaic = settings.options & RENDER_OPT_REMOSAIC_TO_BAYER;
    const bool topologyBeforeBake = bakeGain &&
        (options.outputScale > 1 ||
         (!options.hasCfa && remosaic));
    if (topologyBeforeBake && !DNGDecoder::processHigherCFA(
            dng, options.cfaRepeatSize, options.cfaPhase, mode, remosaic,
            options.outputScale, settings.options & RENDER_OPT_HIGHER_CFA_HQ,
            false, true))
        throw std::runtime_error("Unsupported pre-gain topology conversion for " + source);
    if (hasOpcode2 && !bakeGain && colorOnly &&
        !DNGDecoder::transformGainMaps(dng, false, true, false))
        throw std::runtime_error("Unsupported gain-map color transform for " + source);
    if (hasOpcode2 && !bakeGain &&
        (settings.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) &&
        !DNGDecoder::transformGainMaps(dng, false, false, true))
        throw std::runtime_error("Unsupported gain-map optimization for " + source);
    if (bakeGain && !DNGDecoder::bakeGainMaps(
            dng, settings.options & RENDER_OPT_NORMALIZE_SHADING_MAP, colorOnly,
            settings.options & RENDER_OPT_OPTIMIZE_GAIN_MAPS,
            debugGainMap,
            topologyBeforeBake ? 2 : options.cfaRepeatSize, options.cfaPhase))
        throw std::runtime_error("Unsupported gain-map bake for " + source);
    if (!topologyBeforeBake &&
        (options.cfaRepeatSize > 2 ||
         (options.hasCfa && settings.cameraNativeStaging) ||
         options.outputScale > 1 || remosaic) &&
        !DNGDecoder::processHigherCFA(
            dng, options.cfaRepeatSize, options.cfaPhase, mode, remosaic,
            options.outputScale, settings.options & RENDER_OPT_HIGHER_CFA_HQ))
        throw std::runtime_error("Unsupported CFA/RGB topology conversion for " + source);
    if (topologyBeforeBake && options.hasCfa && options.outputScale > 1 &&
        demosaics(mode) && !remosaic &&
        !DNGDecoder::processHigherCFA(
            dng, 2, options.cfaPhase, mode, false, 1, false))
        throw std::runtime_error("Unsupported post-gain demosaic for " + source);
    bool applyLog = settings.logTransform != LogTransformMode::KeepInput || bakeGain;
    if (options.linearInputBitDepth) {
        uint32_t outputBits = 1;
        const uint32_t quantizationWhite = options.inputQuantizationWhite
            ? options.inputQuantizationWhite : 65535;
        while (outputBits < 16 && ((uint32_t{1} << outputBits) - 1) < quantizationWhite)
            ++outputBits;
        if (settings.logTransform == LogTransformMode::ReduceBy2Bit)
            outputBits = std::max(1u, outputBits - 2);
        else if (settings.logTransform == LogTransformMode::ReduceBy4Bit)
            outputBits = std::max(1u, outputBits - 4);
        else if (settings.logTransform == LogTransformMode::ReduceBy6Bit)
            outputBits = std::max(1u, outputBits - 6);
        else if (settings.logTransform == LogTransformMode::ReduceBy8Bit)
            outputBits = std::max(1u, outputBits - 8);
        applyLog = bakeGain || outputBits < options.linearInputBitDepth;
    }
    if ((settings.options & RENDER_OPT_LOG_TRANSFORM) &&
        !(settings.options & RENDER_OPT_DEBUG_SHADING_MAP) &&
        applyLog &&
        !DNGDecoder::applyLogTransform(
            dng, settings.logTransform, options.inputQuantizationWhite))
        throw std::runtime_error("Could not apply log transform to " + source);
}

bool decodeProcessedDngPreview(
        const std::shared_ptr<std::vector<char>>& dng, PreviewFrame& preview) {
    if (!dng) return false;
    std::vector<uint8_t> bytes(dng->begin(), dng->end());
    // All requested processing is already baked or represented in the
    // canonical frame. Decode without applying a second crop/proxy/gain pass.
    return DNGDecoder::decodePreview(
        std::move(bytes), RenderSettings{}, preview, false);
}

void finalizeDng(std::vector<uint8_t>& dng, const RenderSettings& settings,
                 const DngFinalizeOptions& options) {
    const std::string source = options.sourceName.empty()
        ? std::string("frame") : std::string(options.sourceName);
    const double exposureOffset = configuredExposureOffset(settings);
    if (exposureOffset != 0.0) {
        DNGFrameMetadata metadata;
        if (!DNGDecoder::getColorMetadata(dng, metadata))
            throw std::runtime_error("Could not read " + source + " exposure metadata");
        const double baselineExposure = metadata.baselineExposure + exposureOffset;
        if (!DNGDecoder::updateMetadata(dng, &baselineExposure, nullptr))
            throw std::runtime_error("Could not update " + source + " exposure metadata");
    }
    if (options.isoOverlay &&
        !DNGDecoder::bakeIsoOverlay(dng, *options.isoOverlay))
        throw std::runtime_error("Could not bake ISO overlay into " + source + " DNG");
    if (options.writeTiming &&
        !DNGDecoder::setTimingMetadata(dng, options.frameRate, options.timestamp))
        throw std::runtime_error("Could not update " + source + " DNG timing metadata");
    if (options.packToWhiteLevel && !DNGDecoder::packUncompressedToWhiteLevel(dng))
        throw std::runtime_error("Could not pack " + source + " DNG to its sensor bit depth");
    if (options.gyroflowLensProfile)
        applyGyroflowLensProfile(dng, *options.gyroflowLensProfile);
    if (!options.compression) return;

    const bool compressed = isLossyJpegDct(settings.jxlDistance)
        ? DNGDecoder::compressLossyJPEG(dng)
        : settings.jxlDistance < 0.0f
            ? DNGDecoder::compressLosslessJPEG(dng)
            : DNGDecoder::compressJPEGXL(dng, settings.jxlDistance);
    if (!compressed)
        throw std::runtime_error("Could not compress " + source + " DNG");
}

namespace {
std::vector<double> temporalSmooth(const std::vector<double>& values, int radius) {
    std::vector<double> stable(values.size()), window;
    window.reserve(static_cast<size_t>(radius * 2 + 1));
    for (size_t i = 0; i < values.size(); ++i) {
        const size_t begin = i > static_cast<size_t>(radius) ? i - radius : 0;
        const size_t end = std::min(values.size(), i + static_cast<size_t>(radius) + 1);
        window.assign(values.begin() + begin, values.begin() + end);
        const auto middle = window.begin() + window.size() / 2;
        std::nth_element(window.begin(), middle, window.end());
        stable[i] = *middle;
    }
    std::vector<double> result(values.size());
    const double sigma = std::max(1.0, radius / 2.0);
    for (size_t i = 0; i < stable.size(); ++i) {
        const size_t begin = i > static_cast<size_t>(radius) ? i - radius : 0;
        const size_t end = std::min(stable.size(), i + static_cast<size_t>(radius) + 1);
        double sum = 0.0, weightSum = 0.0;
        for (size_t j = begin; j < end; ++j) {
            const double distance = static_cast<double>(j) - static_cast<double>(i);
            const double weight = std::exp(-0.5 * distance * distance / (sigma * sigma));
            sum += stable[j] * weight;
            weightSum += weight;
        }
        result[i] = sum / weightSum;
    }
    return result;
}
} // namespace

ExposureAnalysis analyzeExposureMetadata(
        const std::vector<ExposureSample>& samples, float frameRate) {
    ExposureAnalysis result;
    if (samples.empty()) return result;
    std::vector<double> effective;
    std::array<std::vector<double>, 3> logNeutrals;
    effective.reserve(samples.size());
    double minimum = std::numeric_limits<double>::max();
    for (const auto& sample : samples) {
        const double camera = std::max(1e-12, sample.iso * sample.exposureSeconds);
        const double value = std::log2(camera) + sample.baselineExposure;
        effective.push_back(value);
        minimum = std::min(minimum, value);
        for (size_t c = 0; c < 3; ++c)
            logNeutrals[c].push_back(std::log(std::max(1e-6f, sample.asShotNeutral[c])));
    }
    const int radius = std::max(1, static_cast<int>(std::lround(
        2.0 * std::max(1.0f, frameRate))));
    const auto smoothExposure = temporalSmooth(effective, radius);
    std::array<std::vector<double>, 3> smoothNeutral;
    for (size_t c = 0; c < 3; ++c) smoothNeutral[c] = temporalSmooth(logNeutrals[c], radius);
    for (size_t i = 0; i < samples.size(); ++i) {
        const double camera = std::log2(std::max(
            1e-12, samples[i].iso * samples[i].exposureSeconds));
        result.normalizedBaseline[samples[i].timestamp] = static_cast<float>(minimum - camera);
        result.smoothedBaseline[samples[i].timestamp] = static_cast<float>(smoothExposure[i] - camera);
        const double green = smoothNeutral[1][i];
        auto& neutral = result.smoothedNeutral[samples[i].timestamp];
        for (size_t c = 0; c < 3; ++c)
            neutral[c] = static_cast<float>(std::exp(smoothNeutral[c][i] - green));
    }
    return result;
}

std::vector<Entry> mapFramesToCfr(
        const std::vector<Entry>& sourceEntries, const std::vector<Timestamp>& timestamps,
        const std::string& baseName, float frameRate, bool convert,
        int& droppedFrames, int& duplicatedFrames) {
    if (sourceEntries.size() != timestamps.size())
        throw std::invalid_argument("CFR source entries and timestamps differ in size");
    droppedFrames = duplicatedFrames = 0;
    std::vector<Entry> output;
    output.reserve(sourceEntries.size() * (convert ? 2 : 1));
    int nextOutput = 0;
    if (convert && !sourceEntries.empty()) {
        size_t previousSource = 0;
        for (size_t i = 0; i < sourceEntries.size(); ++i) {
            const int pts = getFrameNumberFromTimestamp(timestamps[i], timestamps.front(), frameRate);
            if (pts < nextOutput) { ++droppedFrames; continue; }
            while (nextOutput < pts) {
                Entry held = sourceEntries[previousSource];
                held.sourceFrame = static_cast<int>(previousSource);
                held.duplicateFrame = true;
                held.name = constructFrameFilename(baseName, nextOutput++, 6, "dng");
                output.push_back(std::move(held));
                ++duplicatedFrames;
            }
            Entry current = sourceEntries[i];
            current.sourceFrame = static_cast<int>(i);
            current.name = constructFrameFilename(baseName, nextOutput++, 6, "dng");
            output.push_back(std::move(current));
            previousSource = i;
        }
    } else {
        for (size_t i = 0; i < sourceEntries.size(); ++i) {
            Entry entry = sourceEntries[i];
            entry.sourceFrame = static_cast<int>(i);
            entry.name = constructFrameFilename(baseName, nextOutput++, 6, "dng");
            output.push_back(std::move(entry));
        }
    }
    return output;
}

void buildGalleryFrameMap(
        const std::vector<Timestamp>& sourceTimestamps,
        const std::vector<Entry>& mappedEntries,
        std::shared_ptr<const std::vector<int>>& sourceFrameToOutput,
        std::shared_ptr<const std::vector<bool>>& sourceFrameDuplicated) {
    auto outputs = std::make_shared<std::vector<int>>(sourceTimestamps.size(), -1);
    auto duplicated = std::make_shared<std::vector<bool>>(sourceTimestamps.size(), false);
    int outputFrame = 0;
    std::vector<int> occurrences(sourceTimestamps.size(), 0);
    for (const auto& entry : mappedEntries) {
        if (!boost::algorithm::iends_with(entry.name, ".dng")) continue;
        if (entry.sourceFrame >= 0 &&
            entry.sourceFrame < static_cast<int>(sourceTimestamps.size())) {
            const size_t source = static_cast<size_t>(entry.sourceFrame);
            if ((*outputs)[source] < 0)
                (*outputs)[source] = outputFrame;
            ++occurrences[source];
        }
        ++outputFrame;
    }
    for (size_t index = 0; index < occurrences.size(); ++index)
        (*duplicated)[index] = occurrences[index] > 1;
    sourceFrameToOutput = std::move(outputs);
    sourceFrameDuplicated = std::move(duplicated);
}

std::vector<Entry> filterEntries(const std::vector<Entry>& entries, const std::string& filter) {
    if (filter.empty()) return entries;
    std::vector<Entry> filtered;
    std::copy_if(entries.begin(), entries.end(), std::back_inserter(filtered),
        [&](const Entry& entry) { return entry.name.find(filter) != std::string::npos; });
    return filtered;
}

std::optional<Entry> findEntry(const std::vector<Entry>& entries, const std::string& fullPath) {
    const auto relative = boost::filesystem::path(fullPath).relative_path();
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&](const Entry& entry) { return entry.getFullPath() == relative; });
    return found == entries.end() ? std::nullopt : std::optional<Entry>(*found);
}

int outputFrameNumber(const Entry& entry) {
    const auto extension = entry.name.rfind('.');
    if (extension == std::string::npos || extension < 6)
        throw std::invalid_argument("Frame entry has no six-digit sequence number: " + entry.name);
    return std::stoi(entry.name.substr(extension - 6, 6));
}

Timestamp outputTimestamp(
        const Entry& entry, Timestamp sourceTimestamp, Timestamp firstSourceTimestamp,
        float frameRate, bool converted) {
    return converted
        ? static_cast<Timestamp>(std::llround(outputFrameNumber(entry) * 1e9 / frameRate))
        : sourceTimestamp - firstSourceTimestamp;
}

float configuredExposureOffset(const RenderSettings& settings) {
    float result = settings.cameraModel == "Panasonic" ? -2.0f : 0.0f;
    if (!settings.exposureCompensation.empty()) {
        std::string_view input(settings.exposureCompensation);
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
            input.remove_prefix(1);
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
            input.remove_suffix(1);
        float offset = 0.0f;
        const auto parsed = std::from_chars(
            input.data(), input.data() + input.size(), offset,
            std::chars_format::general);
        const char* end = parsed.ptr;
        while (end != input.data() + input.size() &&
               std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (input.data() + input.size() - end == 2 &&
            std::tolower(static_cast<unsigned char>(end[0])) == 'e' &&
            std::tolower(static_cast<unsigned char>(end[1])) == 'v')
            end += 2;
        if (parsed.ec == std::errc{} && end == input.data() + input.size() &&
            std::isfinite(offset))
            result += offset;
    }
    return result;
}

CameraIdentity resolveCameraIdentity(
        const std::string& configuredModel, const std::string& fallbackModel) {
    if (configuredModel == "Blackmagic")
        return {"Blackmagic Pocket Cinema Camera 4K", {}, {}};
    if (configuredModel == "Panasonic")
        return {"Panasonic Varicam RAW", {}, {}};
    if (configuredModel == "Fujifilm" || configuredModel == "Fujifilm X-T5")
        return {"Fujifilm X-T5", "Fujifilm", "X-T5"};
    return {configuredModel.empty() ? fallbackModel : configuredModel, {}, {}};
}

void appendDesktopIni(std::vector<Entry>& entries) {
#ifdef _WIN32
    Entry entry;
    entry.type = EntryType::FILE_ENTRY;
    entry.name = "desktop.ini";
    entry.size = DESKTOP_INI.size();
    entries.push_back(std::move(entry));
#else
    (void) entries;
#endif
}

std::optional<int> readDesktopIni(
        const Entry& entry, size_t pos, size_t len, void* dst,
        const std::function<void(size_t, int)>& result) {
    if (entry.name != "desktop.ini") return std::nullopt;
#ifdef _WIN32
    const size_t count = pos < DESKTOP_INI.size()
        ? std::min(len, DESKTOP_INI.size() - pos) : 0;
    if (count) std::memcpy(dst, DESKTOP_INI.data() + pos, count);
    result(count, 0);
    return 0;
#else
    result(0, -1);
    return -1;
#endif
}

std::vector<GainMap> loadSidecarGainMaps(
        const nlohmann::json& sidecar, size_t frameNumber, const char* field) {
    std::vector<GainMap> maps;
    const nlohmann::json* references = nullptr;
    const nlohmann::json* formats = nullptr;
    const nlohmann::json* payloads = nullptr;
    if (sidecar.contains("dynamic") && sidecar["dynamic"].is_object()) {
        const auto& dynamic = sidecar["dynamic"];
        if (dynamic.contains("frames") && dynamic["frames"].is_array() &&
            frameNumber < dynamic["frames"].size()) {
            const auto& frame = dynamic["frames"][frameNumber];
            if (frame.contains(field) && frame[field].is_array()) {
                references = &frame[field];
                if (dynamic.contains("gainMapFormats")) formats = &dynamic["gainMapFormats"];
                if (dynamic.contains("gainMapPayloads")) payloads = &dynamic["gainMapPayloads"];
            }
        }
    }
    // A top-level reference list is a static, clip-wide override. This keeps
    // hand-authored sidecars compact while per-frame data, when present, wins.
    if (!references && sidecar.contains(field) && sidecar[field].is_array()) {
        references = &sidecar[field];
        if (sidecar.contains("gainMapFormats")) formats = &sidecar["gainMapFormats"];
        if (sidecar.contains("gainMapPayloads")) payloads = &sidecar["gainMapPayloads"];
    }
    if (!references) return maps;
    if (!formats || !formats->is_array() || !payloads || !payloads->is_array())
        throw std::runtime_error("Gain-map sidecar is missing formats or payloads");
    for (const auto& reference : *references) {
        const size_t formatIndex = reference.at("format").get<size_t>();
        const size_t payloadIndex = reference.at("payload").get<size_t>();
        if (formatIndex >= formats->size() || payloadIndex >= payloads->size())
            throw std::runtime_error("Invalid gain-map sidecar reference");
        const auto& format = (*formats)[formatIndex];
        const auto& payload = (*payloads)[payloadIndex];
        const size_t count = payload.at("valueCount").get<size_t>();
        if (count > std::numeric_limits<uint32_t>::max() / sizeof(float))
            throw std::runtime_error("Gain-map sidecar payload is too large");
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
            throw std::runtime_error("Could not decompress gain-map sidecar values");
        GainMap map{};
        map.top = format.at("top").get<uint32_t>();
        map.left = format.at("left").get<uint32_t>();
        map.bottom = format.at("bottom").get<uint32_t>();
        map.right = format.at("right").get<uint32_t>();
        map.coordinateWidth = format.value("coordinateWidth", map.right);
        map.coordinateHeight = format.value("coordinateHeight", map.bottom);
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
        if (!map.width || !map.height || !map.channels || !map.rowPitch || !map.colPitch ||
            map.top >= map.bottom || map.left >= map.right ||
            count != static_cast<size_t>(map.width) * map.height * map.channels)
            throw std::runtime_error("Invalid gain-map sidecar geometry");
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

bool hasSidecarGainMaps(
        const nlohmann::json& sidecar, size_t frameNumber, const char* field) {
    if (sidecar.contains("dynamic") && sidecar["dynamic"].is_object()) {
        const auto& dynamic = sidecar["dynamic"];
        if (dynamic.contains("frames") && dynamic["frames"].is_array() &&
            frameNumber < dynamic["frames"].size()) {
            const auto& frame = dynamic["frames"][frameNumber];
            if (frame.contains(field) && frame[field].is_array()) return true;
        }
    }
    return sidecar.contains(field) && sidecar[field].is_array();
}

void replaceSidecarGainMapOpcodes(
        std::vector<uint8_t>& dng, const nlohmann::json& sidecar,
        size_t frameNumber, bool replaceList2, bool replaceList3) {
    bool applied = false;
    auto replace = [&](const char* field, int opcodeList, bool enabled) {
        if (!enabled || !hasSidecarGainMaps(sidecar, frameNumber, field)) return;
        if (!DNGDecoder::replaceGainMaps(
                dng, opcodeList, loadSidecarGainMaps(sidecar, frameNumber, field)))
            throw std::runtime_error(
                "Could not apply sidecar OpcodeList" + std::to_string(opcodeList) +
                " gain-map override");
        applied = true;
    };
    replace("gainMaps", 2, replaceList2);
    replace("deferredGainMaps", 3, replaceList3);
    if (applied && !DNGDecoder::canonicalizeGainMapOpcodes(dng))
        throw std::runtime_error("Could not canonicalize sidecar gain-map override");
}

nlohmann::json loadSidecarMetadataFile(const boost::filesystem::path& path) {
    if (!boost::filesystem::exists(path)) return {};
    try {
        std::ifstream input(path.string());
        if (!input) throw std::runtime_error("could not open file");
        std::ostringstream contents;
        contents << input.rdbuf();
        return CalibrationData::parseSidecarJson(contents.str());
    } catch (const std::exception& e) {
        spdlog::warn("Could not parse sidecar metadata from {}: {}", path.string(), e.what());
        return {};
    }
}

boost::filesystem::path sidecarPath(const std::string& sourcePath) {
    // A directory selected by a file picker and the same directory supplied
    // with a trailing separator must resolve to the same sidecar. Boost treats
    // the latter as having an empty filename ("clip/" -> "clip/.json").
    boost::filesystem::path source(sourcePath);
    while (source.filename().empty() && source.has_parent_path() &&
           source.parent_path() != source)
        source = source.parent_path();
    return boost::filesystem::is_directory(source)
        ? source / (source.filename().string() + ".json")
        : source.parent_path() / (source.stem().string() + ".json");
}

boost::filesystem::path gyroflowSidecarPath(const std::string& sourcePath) {
    boost::filesystem::path source(sourcePath);
    while (source.filename().empty() && source.has_parent_path())
        source = source.parent_path();
    return boost::filesystem::is_directory(source)
        ? source / (source.filename().string() + "_gyroflow.json")
        : source.parent_path() / (source.stem().string() + "_gyroflow.json");
}

namespace {
std::array<double, 4> solveFour(double normal[4][5]) {
    for (size_t column = 0; column < 4; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < 4; ++row)
            if (std::abs(normal[row][column]) > std::abs(normal[pivot][column]))
                pivot = row;
        if (std::abs(normal[pivot][column]) < 1e-18)
            throw std::runtime_error("Singular Gyroflow warp fit");
        if (pivot != column)
            for (size_t item = column; item < 5; ++item)
                std::swap(normal[pivot][item], normal[column][item]);
        const double scale = normal[column][column];
        for (size_t item = column; item < 5; ++item) normal[column][item] /= scale;
        for (size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = normal[row][column];
            for (size_t item = column; item < 5; ++item)
                normal[row][item] -= factor * normal[column][item];
        }
    }
    return {normal[0][4], normal[1][4], normal[2][4], normal[3][4]};
}

template <typename Basis>
std::array<double, 4> fitWarp(const GyroflowLensProfile& profile, Basis basis,
                              double& rms, double& maximum) {
    constexpr size_t samples = 4097;
    const double focal = 0.5 * (profile.fx + profile.fy);
    const double maxDistance = std::max({
        std::hypot(profile.cx, profile.cy),
        std::hypot(profile.width - profile.cx, profile.cy),
        std::hypot(profile.cx, profile.height - profile.cy),
        std::hypot(profile.width - profile.cx, profile.height - profile.cy)});
    double normal[4][5]{};
    for (size_t sample = 1; sample < samples; ++sample) {
        const double radius = static_cast<double>(sample) / (samples - 1);
        const double theta = std::atan(radius * maxDistance / focal);
        const double theta2 = theta * theta;
        const auto& k = profile.distortion;
        const double target = focal / maxDistance * theta *
            (1.0 + theta2 * (k[0] + theta2 * (k[1] + theta2 * (k[2] + theta2 * k[3]))));
        const auto terms = basis(radius);
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = 0; column < 4; ++column)
                normal[row][column] += terms[row] * terms[column];
            normal[row][4] += terms[row] * target;
        }
    }
    const auto result = solveFour(normal);
    double squared = 0.0;
    maximum = 0.0;
    for (size_t sample = 1; sample < samples; ++sample) {
        const double radius = static_cast<double>(sample) / (samples - 1);
        const double theta = std::atan(radius * maxDistance / focal);
        const double theta2 = theta * theta;
        const auto& k = profile.distortion;
        const double target = focal / maxDistance * theta *
            (1.0 + theta2 * (k[0] + theta2 * (k[1] + theta2 * (k[2] + theta2 * k[3]))));
        const auto terms = basis(radius);
        double fitted = 0.0;
        for (size_t i = 0; i < 4; ++i) fitted += result[i] * terms[i];
        const double error = (fitted - target) * maxDistance;
        squared += error * error;
        maximum = std::max(maximum, std::abs(error));
    }
    rms = std::sqrt(squared / (samples - 1));
    return result;
}
} // namespace

std::optional<GyroflowLensProfile> loadGyroflowLensProfile(
        const boost::filesystem::path& path, bool) {
    if (!boost::filesystem::is_regular_file(path)) return std::nullopt;
    try {
        const auto json = loadSidecarMetadataFile(path);
        if (!json.is_object() || json.value("asymmetrical", false) ||
            !json.contains("fisheye_params"))
            throw std::runtime_error("only symmetrical OpenCV fisheye profiles are supported");
        const auto& dimensions = json.at("calib_dimension");
        const auto& fisheye = json.at("fisheye_params");
        const auto& matrix = fisheye.at("camera_matrix");
        const auto& coefficients = fisheye.at("distortion_coeffs");
        GyroflowLensProfile profile;
        profile.width = dimensions.at("w").get<int>();
        profile.height = dimensions.at("h").get<int>();
        profile.fx = matrix.at(0).at(0).get<double>();
        profile.fy = matrix.at(1).at(1).get<double>();
        profile.cx = matrix.at(0).at(2).get<double>();
        profile.cy = matrix.at(1).at(2).get<double>();
        if (profile.width <= 0 || profile.height <= 0 || profile.fx <= 0.0 ||
            profile.fy <= 0.0 || coefficients.size() != 4 ||
            profile.cx < 0.0 || profile.cx > profile.width ||
            profile.cy < 0.0 || profile.cy > profile.height)
            throw std::runtime_error("invalid dimensions, camera matrix, or coefficients");
        for (size_t i = 0; i < 4; ++i)
            profile.distortion[i] = coefficients.at(i).get<double>();
        profile.dngFisheye = fitWarp(profile, [](double radius) {
            const double theta = std::atan(radius), theta2 = theta * theta;
            return std::array<double, 4>{theta, theta * theta2,
                theta * theta2 * theta2, theta * theta2 * theta2 * theta2};
        }, profile.fisheyeRmsPixels, profile.fisheyeMaxPixels);
        profile.dngRectilinear = fitWarp(profile, [](double radius) {
            const double radius2 = radius * radius;
            return std::array<double, 4>{radius, radius * radius2,
                radius * radius2 * radius2, radius * radius2 * radius2 * radius2};
        }, profile.rectilinearRmsPixels, profile.rectilinearMaxPixels);
        spdlog::info("Loaded Gyroflow profile {}: WarpFisheye RMS/max {:.3f}/{:.3f} px, "
                     "WarpRectilinear RMS/max {:.3f}/{:.3f} px",
                     path.string(), profile.fisheyeRmsPixels, profile.fisheyeMaxPixels,
                     profile.rectilinearRmsPixels, profile.rectilinearMaxPixels);
        return profile;
    } catch (const std::exception& error) {
        spdlog::warn("Ignoring invalid Gyroflow profile {}: {}", path.string(), error.what());
        return std::nullopt;
    }
}

void applyGyroflowLensProfile(
        std::vector<uint8_t>& dng, const GyroflowLensProfile& profile) {
    if (!DNGDecoder::setWarpFisheye(dng, profile.dngFisheye,
            profile.cx / profile.width, profile.cy / profile.height))
        throw std::runtime_error("Could not attach Gyroflow WarpFisheye opcode");
}

void loadSidecar(
        const boost::filesystem::path& path, nlohmann::json& metadata,
        std::optional<CalibrationData>& calibration, bool refresh) {
    struct CachedSidecar {
        nlohmann::json metadata;
        std::optional<CalibrationData> calibration;
        uint64_t lastAccess = 0;
    };
    constexpr size_t maxCachedSidecars = 16;
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, CachedSidecar> cache;
    static uint64_t accessSerial = 0;

    const auto key = boost::filesystem::absolute(path).lexically_normal().string();
    std::lock_guard lock(cacheMutex);
    auto found = cache.find(key);
    if (refresh || found == cache.end()) {
        CachedSidecar parsed;
        parsed.metadata = loadSidecarMetadataFile(path);
        if (!parsed.metadata.empty())
            parsed.calibration = CalibrationData::parse(parsed.metadata);
        parsed.lastAccess = ++accessSerial;
        found = cache.insert_or_assign(key, std::move(parsed)).first;
    } else {
        found->second.lastAccess = ++accessSerial;
    }
    while (cache.size() > maxCachedSidecars) {
        const auto oldest = std::min_element(cache.begin(), cache.end(),
            [](const auto& left, const auto& right) {
                return left.second.lastAccess < right.second.lastAccess;
            });
        if (oldest == cache.end()) break;
        cache.erase(oldest);
    }
    metadata = found->second.metadata;
    calibration = found->second.calibration;
}

std::shared_ptr<std::vector<char>> materializeCached(
        LRUCache& cache, const Entry& entry, bool bypassCache,
        const std::function<std::shared_ptr<std::vector<char>>()>& renderer) {
    if (bypassCache) return renderer();
    if (auto cached = cache.get(entry)) {
        cache.put(entry, cached);
        return cached;
    }
    try {
        auto output = renderer();
        if (!output) throw std::runtime_error("Frame materializer returned no data");
        cache.put(entry, output);
        return output;
    } catch (...) {
        cache.markLoadFailed(entry);
        throw;
    }
}

int readMountedEntry(
        const Entry& entry, size_t pos, size_t len, void* dst,
        const std::function<void(size_t, int)>& result, bool async,
        BS::thread_pool& processingThreadPool,
        const std::function<std::shared_ptr<std::vector<char>>()>& materializer,
        const std::function<std::shared_ptr<std::vector<char>>()>& staticMaterializer,
        int priority) {
    if (const auto desktop = readDesktopIni(entry, pos, len, dst, result)) return *desktop;
    auto copyRange = [=]() -> size_t {
        try {
            const auto data = staticMaterializer ? staticMaterializer() : materializer();
            const size_t count = data && pos < data->size()
                ? std::min(len, data->size() - pos) : 0;
            if (count) std::memcpy(dst, data->data() + pos, count);
            result(count, 0);
            return count;
        } catch (const std::exception& e) {
            spdlog::error("Mounted entry read failed for {}: {}", entry.name, e.what());
            result(0, -1);
            return 0;
        }
    };
    if (staticMaterializer) return static_cast<int>(copyRange());
    if (!boost::algorithm::ends_with(entry.name, ".dng")) {
        result(0, -1);
        return -1;
    }
    auto future = processingThreadPool.submit_task(
        copyRange, static_cast<BS::priority_t>(std::clamp(priority, -32768, 32767)));
    return async ? 0 : static_cast<int>(future.get());
}

void finalize(
    IVirtualFileSystem& filesystem,
    const std::string& destination,
    bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    namespace stdfs = std::filesystem;

    if (!writeFiles && !fileReady)
        throw std::invalid_argument("Streaming finalization requires a frame callback");

    auto entries = filesystem.listFiles("");
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.type != EntryType::FILE_ENTRY || entry.name == "desktop.ini";
    }), entries.end());

    // Callback-only finalization is a DNG frame stream. Ancillary entries such
    // as audio.wav are supplied separately and must not be materialized or
    // written to the caller's destination (which may be a shared temp path).
    if (!writeFiles) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
            return entry.name.size() < 4 ||
                entry.name.substr(entry.name.size() - 4) != ".dng";
        }), entries.end());
    }

    if (options.firstDngFrame) {
        size_t dngsSeen = 0;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            const bool dng = entry.name.size() >= 4 &&
                entry.name.substr(entry.name.size() - 4) == ".dng";
            return dng && dngsSeen++ < options.firstDngFrame;
        }), entries.end());
    }

    auto outputPath = [&](const Entry& entry) {
        stdfs::path path(destination);
        for (const auto& part : entry.pathParts) path /= part;
        return path / entry.name;
    };
    auto isDng = [](const Entry& entry) {
        return entry.name.size() >= 4 && entry.name.substr(entry.name.size() - 4) == ".dng";
    };
    auto readBytes = [](const stdfs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Could not read " + path.string());
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {});
    };
    auto writeBytes = [](const stdfs::path& path, const std::vector<uint8_t>& bytes) {
        stdfs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("Could not write " + path.string());
    };

    constexpr size_t maxInterpolatedGapFrames = 31;
    struct Gap { size_t left, right; std::vector<size_t> frames; };
    std::vector<Gap> gaps;

    // Duplicate detection is useful independently of interpolation: finalized
    // DNGs carry the result in XMP, and Camera Native forwards it to its JSON
    // sidecar. Perform the comparison before rendering so both output paths see
    // the detected flag even when RIFE is disabled.
    if (options.detectDuplicateDngs) {
        size_t detectedCount = 0;
        for (size_t i = 1; i < entries.size(); ++i) {
            if (!isDng(entries[i]) || !isDng(entries[i - 1]) ||
                entries[i].duplicateFrame)
                continue;
            if (filesystem.sourceImagePayloadsEqual(entries[i], entries[i - 1])) {
                entries[i].duplicateFrame = true;
                ++detectedCount;
            }
        }
        spdlog::info("Detected {} duplicated DNG frame(s) by image payload", detectedCount);
    }

    if (options.interpolateDuplicatedFrames) {
        auto duplicated = [&](size_t current, size_t previous) {
            if (entries[current].duplicateFrame) return true;
            if (std::get<int64_t>(entries[current].userData) ==
                std::get<int64_t>(entries[previous].userData)) return true;
            return false;
        };
        for (size_t i = 1; i + 1 < entries.size();) {
            if (entries[i].name == "audio.wav" || entries[i - 1].name == "audio.wav" ||
                !duplicated(i, i - 1)) {
                ++i;
                continue;
            }
            Gap gap{i - 1, i + 1, {}};
            while (gap.right < entries.size() && entries[gap.right].name != "audio.wav" &&
                   duplicated(gap.right, gap.right - 1))
                ++gap.right;
            if (gap.right == entries.size() || entries[gap.right].name == "audio.wav") break;
            for (size_t frame = i; frame < gap.right; ++frame) gap.frames.push_back(frame);
            const size_t right = gap.right;
            if (gap.frames.size() <= maxInterpolatedGapFrames) {
                gaps.push_back(std::move(gap));
            } else {
                spdlog::info("RIFE interpolation skipped a gap containing {} duplicated "
                             "frames (maximum is {})",
                    gap.frames.size(), maxInterpolatedGapFrames);
            }
            i = right;
        }
    }
    const bool interpolate = !gaps.empty();
    if (options.interpolateDuplicatedFrames)
        spdlog::info("RIFE interpolation found {} gap(s) containing {} duplicated frame(s)",
            gaps.size(), [&] { size_t count = 0; for (const auto& gap : gaps) count += gap.frames.size(); return count; }());
    if (interpolate && (options.rifeDirectory.empty() ||
        !stdfs::is_regular_file(stdfs::path(options.rifeDirectory) / "inference_img.py")))
        throw std::runtime_error("RIFE interpolation is enabled, but inference_img.py was not found in the configured RIFE directory");

    std::vector<int> gapForRight(entries.size(), -1);
    std::vector<int> lastGapForMember(entries.size(), -1);
    std::vector<bool> gapMember(entries.size(), false);
    std::vector<bool> syntheticMember(entries.size(), false);
    size_t syntheticCount = 0, delayedCompressionCount = 0;
    for (size_t g = 0; g < gaps.size(); ++g) {
        gapForRight[gaps[g].right] = static_cast<int>(g);
        gapMember[gaps[g].left] = gapMember[gaps[g].right] = true;
        lastGapForMember[gaps[g].left] = lastGapForMember[gaps[g].right] = static_cast<int>(g);
        for (const auto frame : gaps[g].frames) {
            gapMember[frame] = true;
            syntheticMember[frame] = true;
            lastGapForMember[frame] = static_cast<int>(g);
        }
        syntheticCount += gaps[g].frames.size();
    }
    if (jpegCompression)
        for (size_t i = 0; i < entries.size(); ++i)
            if (gapMember[i] && entries[i].name.size() >= 4 &&
                entries[i].name.substr(entries[i].name.size() - 4) == ".dng")
                ++delayedCompressionCount;
    const size_t totalWork = entries.size() + syntheticCount + delayedCompressionCount;
    size_t completed = 0;
    size_t nextReady = 0;
    std::vector<bool> ready(entries.size(), false);
    std::vector<bool> skipped(entries.size(), false);
    std::vector<std::vector<uint8_t>> finalizedDngs(entries.size());
    auto emitReady = [&] {
        while (nextReady < entries.size() && ready[nextReady]) {
            const size_t readyIndex=nextReady++;
            const auto& entry = entries[readyIndex];
            if (fileReady && entry.name.size() >= 4 &&
                entry.name.substr(entry.name.size() - 4) == ".dng" &&
                !skipped[readyIndex]) {
                Timestamp timestamp = 0;
                auto& bytes = finalizedDngs[readyIndex];
                if (bytes.empty() && writeFiles) bytes = readBytes(outputPath(entry));
                if (!DNGDecoder::getTimingMetadata(bytes, timestamp))
                    throw std::runtime_error("Finalized DNG is missing timing metadata: " + entry.name);
                fileReady(bytes, timestamp);
                if (writeFiles || !gapMember[nextReady - 1]) {
                    bytes.clear();
                    bytes.shrink_to_fit();
                }
            }
        }
    };
    auto report = [&](const std::string& label) {
        if (progress && !progress(completed, totalWork, label))
            throw std::runtime_error("Finalization cancelled");
    };
    auto compressDng = [&](size_t index) {
        auto dng = writeFiles
            ? readBytes(outputPath(entries[index])) : finalizedDngs[index];
        const bool ok = isLossyJpegDct(options.jxlDistance)
            ? DNGDecoder::compressLossyJPEG(dng)
            : options.jxlDistance >= 0.0f
                ? DNGDecoder::compressJPEGXL(dng, options.jxlDistance)
                : DNGDecoder::compressLosslessJPEG(dng);
        if (!ok) throw std::runtime_error("Could not compress " + entries[index].name);
        if (writeFiles) writeBytes(outputPath(entries[index]), dng);
        if (fileReady) finalizedDngs[index] = std::move(dng);
    };

    QTemporaryDir staging;
    QProcess rife;
    if (interpolate) {
        if (!staging.isValid()) throw std::runtime_error("Could not create RIFE staging directory");
        const QString code = QString::fromUtf8(R"PY(
import os, sys, types, numpy as np, torch
from torch.nn import functional as F
repo = sys.argv.pop(1)
os.chdir(repo); sys.path.insert(0, repo)
# MotionCam supplies raw tensors directly; RIFE's OpenCV image I/O is unused.
# Avoid importing its binary cv2 wheel, which may target an older NumPy ABI.
sys.modules['cv2'] = types.ModuleType('cv2')
import inference_img as rife
rife.LoadModel(None); rife.StartModel()
print('MOTIONCAM_READY',flush=True)
def load(path,width,height):
    a=np.fromfile(path,dtype='<u2').reshape(height,width,3).astype(np.float32)/65535.0
    t=torch.tensor(a.transpose(2,0,1)).to(rife.DEVICE).unsqueeze(0)
    ph=(height+63)//64*64; pw=(width+63)//64*64
    return F.pad(t,(0,pw-width,0,ph-height)).half() if rife.GPU_FP16 else F.pad(t,(0,pw-width,0,ph-height))
for line in sys.stdin:
    parts=line.rstrip('\n').split('\t'); width,height=int(parts[0]),int(parts[1])
    left,right=parts[2],parts[3]; a,b=load(left,width,height),load(right,width,height)
    for item in parts[4:]:
        ratio,out=item.split('|',1)
        image=rife.RatioSplit(a,b,float(ratio))[0,:,:height,:width]
        pixels=(image.float().clamp(0,1).cpu().numpy().transpose(1,2,0)*65535.0+0.5).astype('<u2')
        pixels.tofile(out)
    del a,b,image,pixels
    if torch.cuda.is_available(): torch.cuda.empty_cache()
    print('MOTIONCAM_DONE',flush=True)
)PY");
        QString python = options.rifePythonExecutable.empty()
            ? QString("python3") : QString::fromStdString(options.rifePythonExecutable);
#ifdef _WIN32
        const stdfs::path venvPython = stdfs::path(options.rifeDirectory) / "venv/Scripts/python.exe";
        const stdfs::path dotVenvPython = stdfs::path(options.rifeDirectory) / ".venv/Scripts/python.exe";
        if (options.rifePythonExecutable.empty()) {
            if (stdfs::exists(venvPython)) python = QString::fromStdString(venvPython.string());
            else if (stdfs::exists(dotVenvPython)) python = QString::fromStdString(dotVenvPython.string());
            else python = "python";
        }
#else
        const stdfs::path venvPython = stdfs::path(options.rifeDirectory) / "venv/bin/python";
        const stdfs::path dotVenvPython = stdfs::path(options.rifeDirectory) / ".venv/bin/python";
        if (options.rifePythonExecutable.empty()) {
            if (stdfs::exists(venvPython)) python = QString::fromStdString(venvPython.string());
            else if (stdfs::exists(dotVenvPython)) python = QString::fromStdString(dotVenvPython.string());
        }
#endif
        rife.setProcessChannelMode(QProcess::MergedChannels);
        rife.start(python, {"-c", code, QString::fromStdString(options.rifeDirectory)}, QIODevice::ReadWrite);
        if (!rife.waitForStarted()) throw std::runtime_error("Could not start the RIFE Python environment");
        QByteArray startupDiagnostic;
        while (!startupDiagnostic.contains("MOTIONCAM_READY")) {
            if (!rife.waitForReadyRead(250) && rife.state() == QProcess::NotRunning) {
                startupDiagnostic += rife.readAll();
                const std::string detail = startupDiagnostic.toStdString();
                spdlog::error("RIFE initialization failed: {}", detail);
                throw std::runtime_error("RIFE initialization failed:\n" + detail);
            }
            startupDiagnostic += rife.readAll();
            if (progress && !progress(0, totalWork, "Loading the RIFE model")) {
                rife.kill();
                rife.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }
        }
    }

    stdfs::create_directories(destination);
    size_t dngIndex = options.firstDngFrame;
    for (size_t index = 0; index < entries.size(); ++index) {
        report("Rendering " + entries[index].name);
        const bool dngEntry=isDng(entries[index]);
        const size_t currentDngIndex=dngIndex;
        if(dngEntry)++dngIndex;
        if(dngEntry&&options.skipDngFrame&&
           options.skipDngFrame(currentDngIndex)){
            ++completed;skipped[index]=true;ready[index]=true;emitReady();continue;
        }
        const bool delayCompression = interpolate && gapMember[index];
        auto data = filesystem.materializeFile(entries[index],
            delayCompression ? false : jpegCompression);
        if (!data) throw std::runtime_error("Failed to render " + entries[index].name);
        std::vector<uint8_t> rendered(data->begin(), data->end());
        if (isDng(entries[index])) {
            if (entries[index].duplicateFrame && !DNGDecoder::markDuplicateFrame(rendered))
                throw std::runtime_error("Could not mark duplicated frame " + entries[index].name);
            if (entries[index].syntheticFrame && !DNGDecoder::markSyntheticFrame(rendered))
                throw std::runtime_error("Could not preserve synthetic frame " + entries[index].name);
        }
        if (writeFiles || !isDng(entries[index]))
            writeBytes(outputPath(entries[index]), rendered);
        if (fileReady && isDng(entries[index]))
            finalizedDngs[index] = rendered;
        ++completed;

        // Gap placeholders are overwritten by RIFE below. Everything else is
        // immutable now and may be consumed by a streaming finalizer.
        if ((!interpolate || !syntheticMember[index]) &&
            (!jpegCompression || !gapMember[index])) {
            ready[index] = true;
            emitReady();
        }

        if (!interpolate || gapForRight[index] < 0) continue;
        const size_t gapIndex = static_cast<size_t>(gapForRight[index]);
        const auto& gap = gaps[gapIndex];
        auto leftDng = writeFiles
            ? readBytes(outputPath(entries[gap.left])) : finalizedDngs[gap.left];
        auto rightDng = writeFiles
            ? readBytes(outputPath(entries[gap.right])) : finalizedDngs[gap.right];
        auto extractRgb = [&](std::vector<uint8_t> dng, const std::string& name,
                              uint32_t& width, uint32_t& height) {
            RenderSettings decodeSettings;
            decodeSettings.options = RENDER_OPT_HIGHER_CFA_HQ;
            decodeSettings.quadBayerOption = QuadBayerMode::Demosaic;
            PreviewFrame decoded;
            if (!DNGDecoder::decodePreview(
                    std::move(dng), decodeSettings, decoded, false))
                throw std::runtime_error("Could not extract RGB pixels from " + name);
            width = decoded.width;
            height = decoded.height;
            return decoded.rgb;
        };
        uint32_t width = 0, height = 0, rightWidth = 0, rightHeight = 0;
        auto leftRgb = extractRgb(leftDng, entries[gap.left].name, width, height);
        auto rightRgb = extractRgb(rightDng, entries[gap.right].name, rightWidth, rightHeight);
        if (width != rightWidth || height != rightHeight)
            throw std::runtime_error("RIFE input dimensions changed within the sequence");
        const stdfs::path stageRoot(staging.path().toStdString());
        const auto leftRaw = stageRoot / "left.rgb16", rightRaw = stageRoot / "right.rgb16";
        writeBytes(leftRaw, leftRgb); writeBytes(rightRaw, rightRgb);
        std::vector<stdfs::path> outputs;
        std::ostringstream command;
        command << width << '\t' << height << '\t' << leftRaw.string() << '\t' << rightRaw.string();
        for (size_t j = 0; j < gap.frames.size(); ++j) {
            outputs.push_back(stageRoot / ("output-" + std::to_string(j) + ".rgb16"));
            command << '\t' << std::setprecision(17)
                    << static_cast<double>(j + 1) / static_cast<double>(gap.frames.size() + 1)
                    << '|' << outputs.back().string();
        }
        command << '\n';
        const QByteArray request = QByteArray::fromStdString(command.str());
        if (rife.write(request) != request.size() || !rife.waitForBytesWritten()) {
            const std::string detail = (rife.readAll() + QByteArray("\n") +
                rife.errorString().toUtf8()).toStdString();
            spdlog::error("Could not send a frame gap to RIFE: {}", detail);
            throw std::runtime_error("Could not send a frame gap to RIFE:\n" + detail);
        }
        QByteArray diagnostic;
        bool done = false;
        while (!done) {
            if (!rife.waitForReadyRead(250) && rife.state() == QProcess::NotRunning)
                throw std::runtime_error("RIFE failed: " + (diagnostic + rife.readAll()).toStdString());
            diagnostic += rife.readAll();
            done = diagnostic.contains("MOTIONCAM_DONE");
            const std::string label = "Interpolating " + std::to_string(gap.frames.size()) +
                " duplicated frame(s) with RIFE";
            if (progress && !progress(completed, totalWork, label)) {
                rife.kill();
                rife.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }
        }
        for (size_t j = 0; j < gap.frames.size(); ++j) {
            const size_t frame = gap.frames[j];
            auto dng = writeFiles
                ? readBytes(outputPath(entries[frame])) : finalizedDngs[frame];
            int repeat = 0;
            std::array<uint8_t, 4> phase{};
            const bool remosaic = DNGDecoder::getCFAMetadata(dng, repeat, phase);
            if (!DNGDecoder::ensureUncompressed(dng) ||
                (remosaic && !DNGDecoder::processHigherCFA(
                    dng, repeat, phase, QuadBayerMode::Demosaic, false)))
                throw std::runtime_error("Could not prepare synthesized DNG " + entries[frame].name);
            const double ratio = static_cast<double>(j + 1) / static_cast<double>(gap.frames.size() + 1);
            const auto rgb = readBytes(outputs[j]);
            if (!DNGDecoder::replaceNormalizedRGB16(dng, rgb, width, height) ||
                !DNGDecoder::interpolateFrameMetadata(dng, leftDng, rightDng, ratio) ||
                (remosaic && (!DNGDecoder::processHigherCFA(
                    dng, 2, phase, QuadBayerMode::Demosaic, true) ||
                    !DNGDecoder::packUncompressedToWhiteLevel(dng))) ||
                !DNGDecoder::markSyntheticFrame(dng))
                throw std::runtime_error("Could not rebuild synthesized DNG " + entries[frame].name);
            if (writeFiles) writeBytes(outputPath(entries[frame]), dng);
            if (fileReady) finalizedDngs[frame] = dng;
            if (!jpegCompression) ready[frame] = true;
            ++completed;
            report("Rebuilt interpolated frame " + entries[frame].name);
        }
        emitReady();
        leftRgb.clear(); leftRgb.shrink_to_fit();
        rightRgb.clear(); rightRgb.shrink_to_fit();
        for (const auto& output : outputs) stdfs::remove(output);
        stdfs::remove(leftRaw); stdfs::remove(rightRaw);
        if (jpegCompression) {
            std::vector<size_t> members{gap.left};
            members.insert(members.end(), gap.frames.begin(), gap.frames.end());
            members.push_back(gap.right);
            for (const auto member : members) {
                if (lastGapForMember[member] != static_cast<int>(gapIndex)) continue;
                report("Compressing " + entries[member].name);
                compressDng(member);
                ready[member] = true;
                ++completed;
            }
            emitReady();
        }
        if (!writeFiles) {
            std::vector<size_t> releasable{gap.left};
            releasable.insert(releasable.end(), gap.frames.begin(), gap.frames.end());
            releasable.push_back(gap.right);
            for (const auto member : releasable) {
                if (lastGapForMember[member] != static_cast<int>(gapIndex)) continue;
                finalizedDngs[member].clear();
                finalizedDngs[member].shrink_to_fit();
            }
        }
    }
    if (interpolate) {
        rife.closeWriteChannel();
        if (!rife.waitForFinished(5000)) { rife.kill(); rife.waitForFinished(); }
    }
    if (progress) progress(totalWork, totalWork, "Finalization complete");
}

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
    if (frames.size() < 2) {
        return {0,0,0,0,0,0};
    }

    double avgDuration = 0.0;
    int validFrames = 0;
    std::vector<double> durations;
    durations.reserve(frames.size() - 1);

    for (size_t i = 1; i < frames.size(); ++i) {
        double duration = static_cast<double>(frames[i] - frames[i - 1]);
        if (duration > 0) {
            avgDuration += (duration - avgDuration) / (validFrames + 1);
            durations.push_back(duration);
            validFrames++;
        }
    }

    if (validFrames == 0) {
        return {0,0,0,0,0,0};
    }

    std::sort(durations.begin(), durations.end());

    // Helper for percentile (with linear interpolation)
    auto percentile = [&](double p) -> double {
        if (durations.empty()) return 0.0;
        double pos = p * (durations.size() - 1);
        size_t idx = static_cast<size_t>(pos);
        double frac = pos - idx;
        if (idx + 1 < durations.size()) {
            return durations[idx] * (1.0 - frac) + durations[idx + 1] * frac;
        }
        return durations.back();
    };

    // Duration percentiles (in nanoseconds)
    double minDur = durations.front();
    double q1Dur   = percentile(0.25);
    double medianDur = percentile(0.5);
    double q3Dur   = percentile(0.75);
    double maxDur  = durations.back();

    // Convert durations → frame rates (FPS)
    auto toFps = [](double dur) -> float {
        return dur > 0.0 ? static_cast<float>(1e9 / dur) : 0.0f;
    };

    return {
        toFps(maxDur),  // minFrameRate (worst)
        toFps(q3Dur),   // lower quartile
        toFps(medianDur),
        toFps(q1Dur),   // upper quartile
        toFps(minDur),  // max FPS (best)
        toFps(avgDuration)
    };

    /*double medianDuration;
    size_t mid = durations.size() / 2;
    if (durations.size() % 2 == 0) {
        medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
    } else {
        medianDuration = durations[mid];
    }

    return {
        static_cast<float>(1000000000.0 / medianDuration),
        static_cast<float>(1000000000.0 / avgDuration)
    };*/
}

float determineCFRTarget(FrameRateInfo fpsInfo, const CFRTarget& cfrTarget, bool applyCFRConversion) {
    if (!applyCFRConversion) {
        if (cfrTarget.mode == CFRMode::Custom) {
            return cfrTarget.customValue;
        }
        return fpsInfo.averageFrameRate;
    }
    
    switch (cfrTarget.mode) {
        case CFRMode::Disabled:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::PreferInteger:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 24.0f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 30.0f;
            else if (fpsInfo.medianFrameRate < 49.0) return 48.0f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 60.0f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 120.0f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 120.0f;
            else return 60.0f;
            
        case CFRMode::PreferDropFrame:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 23.976f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 29.97f;
            else if (fpsInfo.medianFrameRate < 49.0) return 47.952f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 59.94f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 119.88f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 119.88f;
            else return 59.94f;
            
        case CFRMode::MedianSlowMotion:
            return fpsInfo.medianFrameRate;
            
        case CFRMode::AverageTesting:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::Custom:
            return cfrTarget.customValue;
            
        default:
            return fpsInfo.medianFrameRate;
    }
}

int getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
    if (frameRate <= 0) {
        return -1;
    }

    int64_t timeDifference = timestamp - referenceTimestamp;
    if (timeDifference < 0) {
        return -1;
    }

    double nanosecondsPerFrame = 1000000000.0 / frameRate;
    return static_cast<int>(std::round(timeDifference / nanosecondsPerFrame));
}

std::string constructFrameFilename(
    const std::string& baseName,
    int frameNumber,
    int padding,
    const std::string& extension)
{
    std::ostringstream oss;
    oss << baseName;
    oss << std::setfill('0') << std::setw(padding) << frameNumber;
    
    if (!extension.empty()) {
        if (extension[0] != '.') {
            oss << '.';
        }
        oss << extension;
    }
    
    return oss.str();
}

std::string extractFilenameWithoutExtension(const std::string& fullPath) {
    boost::filesystem::path p(fullPath);
    return p.stem().string();
}

int getScaleFromOptions(FileRenderOptions options, int draftScale) {
    if (options & RENDER_OPT_DRAFT)
        return draftScale;
    return 1;
}

void syncAudio(
    Timestamp videoTimestamp,
    std::vector<AudioChunk>& audioChunks,
    int sampleRate,
    int numChannels)
{
    if (audioChunks.empty()) {
        return;
    }

    auto audioVideoDriftMs = (audioChunks[0].timestamp - videoTimestamp) * 1e-6f;
    if (std::abs(audioVideoDriftMs) > 1000) {
        spdlog::warn("Audio drift too large, not syncing audio");
        return;
    }

    AudioSampleFormat format = audioChunks[0].format;

    if (audioVideoDriftMs > 0) {
        int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
        int samplesToRemove = audioFramesToRemove * numChannels;

        int samplesRemoved = 0;
        auto it = audioChunks.begin();

        while (it != audioChunks.end() && samplesRemoved < samplesToRemove) {
            int remainingSamplesToRemove = samplesToRemove - samplesRemoved;

            size_t chunkSize = it->sampleCount();
            if (chunkSize <= static_cast<size_t>(remainingSamplesToRemove)) {
                samplesRemoved += chunkSize;
                it = audioChunks.erase(it);
            }
            else {
                if (format == AudioSampleFormat::Float32) {
                    it->float32Data.erase(
                        it->float32Data.begin(),
                        it->float32Data.begin() + remainingSamplesToRemove);
                } else {
                    it->int16Data.erase(
                        it->int16Data.begin(),
                        it->int16Data.begin() + remainingSamplesToRemove);
                }
                it->timestamp += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                break;
            }
        }
    }
    else {
        auto silenceDuration = -audioVideoDriftMs;
        int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
        int silenceSamples = silenceFrames * numChannels;

        AudioChunk silenceChunk;
        silenceChunk.timestamp = videoTimestamp;
        silenceChunk.format = format;
        if (format == AudioSampleFormat::Float32) {
            silenceChunk.float32Data.resize(silenceSamples, 0.0f);
        } else {
            silenceChunk.int16Data.resize(silenceSamples, 0);
        }

        audioChunks.insert(audioChunks.begin(), std::move(silenceChunk));

        for (auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
            it->timestamp += silenceDuration;
        }
    }
}

std::string getDisplayDataType(bool sourceRgb, int cfaSize) {
    if (cfaSize > 2)
        return "Higher CFA " + std::to_string(cfaSize) + "x" + std::to_string(cfaSize);
    if (cfaSize == 2 || !sourceRgb)
        return "Bayer CFA";
    return "RGB";
}

FileInfo makeFileInfo(
        const FrameRateInfo& frameRateInfo, float fps, int totalFrames,
        int droppedFrames, int duplicatedFrames, int width, int height) {
    FileInfo info{};
    info.frameRateInfo = frameRateInfo;
    info.fps = fps;
    info.totalFrames = totalFrames;
    info.droppedFrames = droppedFrames;
    info.duplicatedFrames = duplicatedFrames;
    info.width = width;
    info.height = height;
    return info;
}

std::unordered_map<Timestamp, size_t> indexTimestamps(
        const std::vector<Timestamp>& timestamps) {
    std::unordered_map<Timestamp, size_t> result;
    result.reserve(timestamps.size());
    for (size_t index = 0; index < timestamps.size(); ++index)
        result[timestamps[index]] = index;
    return result;
}

std::string getDisplayDataLevels(
    float dynWhiteLevel, std::array<float, 4> dynBlackLevel, 
    float statWhiteLevel, std::array<float, 4> statBlackLevel, 
    std::string levels, std::string logTransform,
    bool applyShadingMap, bool normalizeShadingMap,
    uint32_t inputBitDepth) {

    const auto resolvedLevels = resolveDataLevels(
        levels, dynWhiteLevel, dynBlackLevel, statWhiteLevel, statBlackLevel);
    float srcWhiteLevel = resolvedLevels.white;
    std::array<float, 4> srcBlackLevel = resolvedLevels.black;

    float dstWhiteLevel = srcWhiteLevel;
    std::array<float, 4> dstBlackLevel = srcBlackLevel;

    const auto separator = levels.find('/');
    const std::string selectedWhite = separator == std::string::npos
        ? levels : levels.substr(0, separator);
    const bool usesSourceWhite = selectedWhite.empty() || selectedWhite == "Dynamic" ||
                                 selectedWhite == "Static";
    int useBits = inputBitDepth > 0 && usesSourceWhite
        ? static_cast<int>(std::min<uint32_t>(16, inputBitDepth))
        : std::min(16, static_cast<int>(std::ceil(std::log2(srcWhiteLevel + 1))));

    if (logTransform.empty()) {
        if (applyShadingMap) {
            // Gain-map baking sizes its expanded range from the selected white
            // level, even when a DNG stores those samples in a wider container.
            useBits = std::min(16, static_cast<int>(
                std::ceil(std::log2(srcWhiteLevel + 1))));
            std::array<double, 4> sourceBlack{};
            std::copy(srcBlackLevel.begin(), srcBlackLevel.end(), sourceBlack.begin());
            const auto bakeLevels = planLinearGainBake(
                srcWhiteLevel, sourceBlack, normalizeShadingMap);
            useBits = static_cast<int>(bakeLevels.destinationBits);
            dstWhiteLevel = static_cast<float>(bakeLevels.destinationWhite);
            for (size_t channel = 0; channel < dstBlackLevel.size(); ++channel)
                dstBlackLevel[channel] = static_cast<float>(bakeLevels.destinationBlack[channel]);
        }
    } else {
        if (logTransform == "Reduce by 2bit" || logTransform == "Reduce by 2bit lq")
            useBits -= 2;
        if (logTransform == "Reduce by 4bit" || logTransform == "Reduce by 4bit lq")
            useBits -= 4;
        if (logTransform == "Reduce by 6bit" || logTransform == "Reduce by 6bit lq")
            useBits -= 6;
        if (logTransform == "Reduce by 8bit" || logTransform == "Reduce by 8bit lq")
            useBits -= 8;
        useBits = std::clamp(useBits, 1, 16);
        dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        for (auto& v : dstBlackLevel)
            v = 0;
    }    
    
    std::string result = std::to_string(static_cast<int>(srcWhiteLevel)) + "/" + 
                         std::to_string(static_cast<int>(srcBlackLevel[0]));    // Build levels info string
    
    if (srcWhiteLevel != dstWhiteLevel || srcBlackLevel[0] != dstBlackLevel[0]) {
        result += " -> " + std::to_string(static_cast<int>(dstWhiteLevel)) + "/" + 
                           std::to_string(static_cast<int>(dstBlackLevel[0]));
    }       // Show transformation if levels changed
    
    result += " " + std::to_string(std::min(16, useBits)) + "b";
    if (!logTransform.empty()) 
        result += " log";    

    return result;
}

} // namespace vfs

MountedDngSource::MountedDngSource(
        LRUCache& cache, BS::thread_pool& processingThreadPool) :
        mCache(cache), mProcessingThreadPool(processingThreadPool) {}

std::vector<Entry> MountedDngSource::listFiles(const std::string& filter) const {
    std::lock_guard<std::mutex> lock(mMutex);
    return vfs::filterEntries(mFiles, filter);
}

std::optional<Entry> MountedDngSource::findEntry(const std::string& fullPath) const {
    std::lock_guard<std::mutex> lock(mMutex);
    return vfs::findEntry(mFiles, fullPath);
}

int MountedDngSource::readPriority(const Entry& entry) const {
    return vfs::outputFrameNumber(entry);
}

std::function<std::shared_ptr<std::vector<char>>()>
MountedDngSource::staticMaterializer(const Entry&) {
    return {};
}

int MountedDngSource::readFile(
        const Entry& entry, size_t pos, size_t len, void* dst,
        std::function<void(size_t, int)> result, bool async) {
    return vfs::readMountedEntry(
        entry, pos, len, dst, result, async, mProcessingThreadPool,
        [this, entry] { return materializeFile(entry, false); },
        staticMaterializer(entry), readPriority(entry));
}

} // namespace motioncam

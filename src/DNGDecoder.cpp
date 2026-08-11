#include "DNGDecoder.h"
#include <spdlog/spdlog.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <set>
#include "liblj92/lj92.h"

#ifdef _MSC_VER
#include <stdlib.h>
#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#endif

namespace motioncam {

namespace {
    // DNG tag constants
    constexpr uint16_t TIFF_TAG_OPCODE_LIST_2 = 51009;
    constexpr uint16_t TIFF_TAG_OPCODE_LIST_3 = 51022;
    constexpr uint32_t OPCODE_GAIN_MAP = 9;
    
    // TIFF header constants
    constexpr uint16_t TIFF_LITTLE_ENDIAN = 0x4949;
    constexpr uint16_t TIFF_BIG_ENDIAN = 0x4D4D;
    constexpr uint16_t TIFF_MAGIC = 42;

    constexpr uint16_t TIFF_TAG_EXPOSURE_TIME = 33434;
    constexpr uint16_t TIFF_TAG_EXIF_IFD = 34665;
    constexpr uint16_t TIFF_TAG_ISO = 34855;
    constexpr uint16_t TIFF_TAG_SUB_IFDS = 330;
    constexpr uint16_t TIFF_TAG_AS_SHOT_NEUTRAL = 50728;
    constexpr uint16_t TIFF_TAG_BASELINE_EXPOSURE = 50730;
    constexpr uint16_t TIFF_TYPE_SHORT = 3;
    constexpr uint16_t TIFF_TYPE_LONG = 4;
    constexpr uint16_t TIFF_TYPE_RATIONAL = 5;
    constexpr uint16_t TIFF_TYPE_SRATIONAL = 10;
    constexpr uint16_t TIFF_TAG_IMAGE_WIDTH = 256;
    constexpr uint16_t TIFF_TAG_IMAGE_HEIGHT = 257;
    constexpr uint16_t TIFF_TAG_BITS_PER_SAMPLE = 258;
    constexpr uint16_t TIFF_TAG_COMPRESSION = 259;
    constexpr uint16_t TIFF_TAG_PHOTOMETRIC = 262;
    constexpr uint16_t TIFF_TAG_STRIP_OFFSETS = 273;
    constexpr uint16_t TIFF_TAG_ROWS_PER_STRIP = 278;
    constexpr uint16_t TIFF_TAG_STRIP_BYTE_COUNTS = 279;
    constexpr uint16_t TIFF_TAG_BLACK_LEVEL = 50714;
    constexpr uint16_t TIFF_TAG_WHITE_LEVEL = 50717;
    constexpr uint16_t TIFF_TAG_LINEARIZATION_TABLE = 50712;
    constexpr uint16_t TIFF_PHOTOMETRIC_CFA = 32803;
    constexpr uint16_t TIFF_COMPRESSION_NONE = 1;
    constexpr uint16_t TIFF_COMPRESSION_JPEG = 7;

    uint16_t read16(const uint8_t* p, bool little) {
        return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                      : static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
    uint32_t read32(const uint8_t* p, bool little) {
        return little
            ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
            : static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    }
    uint32_t readBE32(const uint8_t* p) { return read32(p, false); }
    float readBEFloat(const uint8_t* p) {
        const uint32_t bits = readBE32(p);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    double readBEDouble(const uint8_t* p) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void write32(uint8_t* p, uint32_t value, bool little) {
        for (int i = 0; i < 4; ++i) {
            const int shift = little ? i * 8 : (3 - i) * 8;
            p[i] = static_cast<uint8_t>((value >> shift) & 0xff);
        }
    }
    void write16(uint8_t* p, uint16_t value, bool little) {
        if (little) {
            p[0] = static_cast<uint8_t>(value & 0xff);
            p[1] = static_cast<uint8_t>(value >> 8);
        } else {
            p[0] = static_cast<uint8_t>(value >> 8);
            p[1] = static_cast<uint8_t>(value & 0xff);
        }
    }

    struct TiffEntry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        size_t valueOffset;
        uint32_t ifdOffset;
        size_t entryOffset;
    };

    std::vector<TiffEntry> findTiffEntries(const std::vector<uint8_t>& data, bool& little) {
        std::vector<TiffEntry> result;
        if (data.size() < 8) return result;
        const uint16_t order = read16(data.data(), true);
        if (order != TIFF_LITTLE_ENDIAN && order != TIFF_BIG_ENDIAN) return result;
        little = order == TIFF_LITTLE_ENDIAN;
        if (read16(data.data() + 2, little) != TIFF_MAGIC) return result;
        std::vector<uint32_t> pending = {read32(data.data() + 4, little)};
        std::set<uint32_t> visited;
        while (!pending.empty()) {
            const uint32_t ifd = pending.back();
            pending.pop_back();
            if (!ifd || !visited.insert(ifd).second || ifd + 2 > data.size()) continue;
            const uint16_t count = read16(data.data() + ifd, little);
            const size_t entriesEnd = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(count) * 12;
            if (entriesEnd + 4 > data.size()) continue;
            for (uint16_t i = 0; i < count; ++i) {
                const size_t pos = static_cast<size_t>(ifd) + 2 + i * 12;
                const uint16_t tag = read16(data.data() + pos, little);
                const uint16_t type = read16(data.data() + pos + 2, little);
                const uint32_t itemCount = read32(data.data() + pos + 4, little);
                const uint32_t rawOffset = read32(data.data() + pos + 8, little);
                size_t typeSize = (type == TIFF_TYPE_SHORT) ? 2 :
                                  (type == TIFF_TYPE_LONG ? 4 :
                                  ((type == TIFF_TYPE_RATIONAL || type == TIFF_TYPE_SRATIONAL) ? 8 : 1));
                const size_t bytes = typeSize * static_cast<size_t>(itemCount);
                const size_t valueOffset = bytes <= 4 ? pos + 8 : rawOffset;
                if (valueOffset <= data.size() && bytes <= data.size() - valueOffset)
                    result.push_back({tag, type, itemCount, valueOffset, ifd, pos});
                if (tag == TIFF_TAG_EXIF_IFD && type == TIFF_TYPE_LONG && itemCount == 1)
                    pending.push_back(rawOffset);
                if (tag == TIFF_TAG_SUB_IFDS && (type == TIFF_TYPE_LONG)) {
                    for (uint32_t j = 0; j < itemCount && valueOffset + j * 4 + 4 <= data.size(); ++j)
                        pending.push_back(read32(data.data() + valueOffset + j * 4, little));
                }
            }
            pending.push_back(read32(data.data() + entriesEnd, little));
        }
        return result;
    }

    double readRational(const std::vector<uint8_t>& data, const TiffEntry& entry,
                        uint32_t index, bool little) {
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) * 8;
        const uint32_t denominator = read32(data.data() + pos + 4, little);
        if (!denominator) return 0.0;
        if (entry.type == TIFF_TYPE_SRATIONAL)
            return static_cast<double>(static_cast<int32_t>(read32(data.data() + pos, little))) /
                   static_cast<int32_t>(denominator);
        return static_cast<double>(read32(data.data() + pos, little)) / denominator;
    }

    void writeRational(std::vector<uint8_t>& data, const TiffEntry& entry,
                       uint32_t index, double value, bool little) {
        constexpr int32_t denominator = 1000000;
        const double scaled = std::round(value * denominator);
        const int32_t numerator = static_cast<int32_t>(std::clamp(
            scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())));
        const size_t pos = entry.valueOffset + static_cast<size_t>(index) * 8;
        write32(data.data() + pos, static_cast<uint32_t>(numerator), little);
        write32(data.data() + pos + 4, denominator, little);
    }

    bool addMissingMetadataEntries(std::vector<uint8_t>& data,
                                   bool addBaseline, bool addNeutral, bool little) {
        if (!addBaseline && !addNeutral) return true;
        const uint32_t oldIfd = read32(data.data() + 4, little);
        if (oldIfd + 2 > data.size()) return false;
        const uint16_t oldCount = read16(data.data() + oldIfd, little);
        const size_t oldEntriesEnd = static_cast<size_t>(oldIfd) + 2 + static_cast<size_t>(oldCount) * 12;
        if (oldEntriesEnd + 4 > data.size() || data.size() > std::numeric_limits<uint32_t>::max())
            return false;

        if (data.size() & 1u) data.push_back(0);
        const uint32_t newIfd = static_cast<uint32_t>(data.size());
        const uint16_t newCount = static_cast<uint16_t>(oldCount + addBaseline + addNeutral);
        const size_t tableSize = 2 + static_cast<size_t>(newCount) * 12 + 4;
        const size_t externalSize = (addBaseline ? 8 : 0) + (addNeutral ? 24 : 0);
        data.resize(data.size() + tableSize + externalSize, 0);
        std::vector<std::array<uint8_t, 12>> newEntries;
        newEntries.reserve(newCount);
        for (uint16_t i = 0; i < oldCount; ++i) {
            std::array<uint8_t, 12> entry{};
            std::memcpy(entry.data(), data.data() + oldIfd + 2 + static_cast<size_t>(i) * 12, 12);
            newEntries.push_back(entry);
        }
        uint32_t valuePos = static_cast<uint32_t>(static_cast<size_t>(newIfd) + tableSize);
        auto addEntry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t bytes) {
            std::array<uint8_t, 12> entry{};
            write16(entry.data(), tag, little);
            write16(entry.data() + 2, type, little);
            write32(entry.data() + 4, count, little);
            write32(entry.data() + 8, valuePos, little);
            newEntries.push_back(entry);
            valuePos += bytes;
        };
        if (addBaseline) addEntry(TIFF_TAG_BASELINE_EXPOSURE, TIFF_TYPE_SRATIONAL, 1, 8);
        if (addNeutral) addEntry(TIFF_TAG_AS_SHOT_NEUTRAL, TIFF_TYPE_RATIONAL, 3, 24);
        std::sort(newEntries.begin(), newEntries.end(), [little](const auto& a, const auto& b) {
            return read16(a.data(), little) < read16(b.data(), little);
        });
        write16(data.data() + newIfd, newCount, little);
        size_t entryPos = static_cast<size_t>(newIfd) + 2;
        for (const auto& entry : newEntries) {
            std::memcpy(data.data() + entryPos, entry.data(), entry.size());
            entryPos += entry.size();
        }
        std::memcpy(data.data() + entryPos, data.data() + oldEntriesEnd, 4);
        write32(data.data() + 4, newIfd, little);
        return true;
    }
}

DNGDecoder::DNGDecoder(const std::string& sequencePath) 
    : mSequencePath(sequencePath) {
    
    spdlog::info("DNGDecoder: Initializing for {}", sequencePath);
    analyzeSequence();
}

DNGDecoder::~DNGDecoder() {
    spdlog::debug("DNGDecoder: Cleanup completed");
}

bool DNGDecoder::isDNGSequence(const std::string& path) {
    boost::filesystem::path p(path);
    
    // Check if it's a directory containing DNG files
    if (boost::filesystem::is_directory(p)) {
        boost::filesystem::directory_iterator end;
        for (boost::filesystem::directory_iterator it(p); it != end; ++it) {
            if (it->path().extension() == ".dng") {
                return true;
            }
        }
    }
    
    // Check if it's a single DNG file (part of sequence)
    if (p.extension() == ".dng" && boost::filesystem::exists(p)) {
        return true;
    }
    
    return false;
}

void DNGDecoder::analyzeSequence() {
    boost::filesystem::path sequencePath(mSequencePath);
    
    if (boost::filesystem::is_directory(sequencePath)) {
        mSequenceInfo.basePath = mSequencePath;
    } else {
        mSequenceInfo.basePath = sequencePath.parent_path().string();
    }
    
    findDNGFiles();
    extractTimestampsFromFilenames();
    
    if (!mFrames.empty()) {
        mSequenceInfo.totalFrames = mFrames.size();
        mSequenceInfo.width = mFrames[0].width;
        mSequenceInfo.height = mFrames[0].height;
        
        // Calculate FPS from timestamps if available
        if (mFrames.size() > 1) {
            double totalDuration = (mFrames.back().timestamp - mFrames.front().timestamp) / 1000000000.0;
            mSequenceInfo.fps = (mFrames.size() - 1) / totalDuration;
        } else {
            mSequenceInfo.fps = 30.0; // Default
        }
    }
    
    spdlog::info("DNGDecoder: Found {} DNG files, {}x{} @ {:.2f}fps", 
                 mSequenceInfo.totalFrames, mSequenceInfo.width, mSequenceInfo.height, mSequenceInfo.fps);
}

void DNGDecoder::findDNGFiles() {
    boost::filesystem::path basePath(mSequenceInfo.basePath);
    
    if (!boost::filesystem::exists(basePath) || !boost::filesystem::is_directory(basePath)) {
        throw std::runtime_error("Invalid DNG sequence path: " + mSequenceInfo.basePath);
    }
    
    std::vector<std::string> dngFiles;
    boost::filesystem::directory_iterator end;
    
    for (boost::filesystem::directory_iterator it(basePath); it != end; ++it) {
        if (it->path().extension() == ".dng") {
            dngFiles.push_back(it->path().string());
        }
    }
    
    if (dngFiles.empty()) {
        throw std::runtime_error("No DNG files found in: " + mSequenceInfo.basePath);
    }
    
    // Sort files by name
    std::sort(dngFiles.begin(), dngFiles.end());
    
    // Create frame info for each DNG file
    mFrames.clear();
    mFrames.reserve(dngFiles.size());
    
    for (size_t i = 0; i < dngFiles.size(); ++i) {
        DNGFrameInfo frameInfo;
        frameInfo.frameNumber = static_cast<int>(i);
        frameInfo.filePath = dngFiles[i];
        frameInfo.width = 1920;  // Will be updated when reading actual DNG
        frameInfo.height = 1080;
        frameInfo.hasGainMap = false;
        frameInfo.timestamp = static_cast<Timestamp>(i * 1000000000.0 / 30.0); // Default timing
        
        mFrames.push_back(frameInfo);
    }
}

void DNGDecoder::extractTimestampsFromFilenames() {
    // Try to extract frame numbers from filenames for better timing
    boost::regex frameNumberRegex(R"((\d{6,}))");
    boost::smatch match;
    
    for (auto& frame : mFrames) {
        boost::filesystem::path p(frame.filePath);
        std::string filename = p.stem().string();
        
        if (boost::regex_search(filename, match, frameNumberRegex)) {
            int extractedFrameNumber = std::stoi(match[1].str());
            frame.frameNumber = extractedFrameNumber;
            
            // Update timestamp based on extracted frame number
            frame.timestamp = static_cast<Timestamp>(extractedFrameNumber * 1000000000.0 / 30.0);
        }
    }
    
    // Sort frames by timestamp
    std::sort(mFrames.begin(), mFrames.end(), 
              [](const DNGFrameInfo& a, const DNGFrameInfo& b) {
                  return a.timestamp < b.timestamp;
              });
}

bool DNGDecoder::extractFrame(int frameNumber, std::vector<uint8_t>& dngData) {
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    const DNGFrameInfo& frameInfo = mFrames[frameNumber];
    return readDNGFile(frameInfo.filePath, dngData);
}

bool DNGDecoder::extractFrameByTimestamp(Timestamp timestamp, std::vector<uint8_t>& dngData) {
    // Find frame with closest timestamp
    auto it = std::lower_bound(mFrames.begin(), mFrames.end(), timestamp,
                              [](const DNGFrameInfo& frame, Timestamp ts) {
                                  return frame.timestamp < ts;
                              });
    
    if (it == mFrames.end()) {
        it = mFrames.end() - 1;
    }
    
    int frameNumber = static_cast<int>(std::distance(mFrames.begin(), it));
    return extractFrame(frameNumber, dngData);
}

bool DNGDecoder::getGainMap(int frameNumber, GainMap& gainMap) {
    if (frameNumber < 0 || frameNumber >= static_cast<int>(mFrames.size())) {
        return false;
    }
    
    // Check cache first
    auto cacheIt = mGainMapCache.find(frameNumber);
    if (cacheIt != mGainMapCache.end()) {
        gainMap = cacheIt->second;
        return true;
    }
    
    const DNGFrameInfo& frameInfo = mFrames[frameNumber];
    
    if (readDNGGainMap(frameInfo.filePath, gainMap)) {
        mGainMapCache[frameNumber] = gainMap;
        return true;
    }
    
    return false;
}

bool DNGDecoder::getGainMaps(int frameNumber, std::vector<GainMap>& gainMaps) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    bool little = true;
    for (const auto& entry : findTiffEntries(data, little)) {
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2 && entry.count &&
            parseOpcodeGainMaps(data.data() + entry.valueOffset, entry.count, gainMaps))
            return true;
    }
    return false;
}

bool DNGDecoder::getFrameMetadata(int frameNumber, DNGFrameMetadata& metadata) {
    std::vector<uint8_t> data;
    if (!extractFrame(frameNumber, data)) return false;
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    for (const auto& entry : entries) {
        if (entry.tag == TIFF_TAG_EXPOSURE_TIME && entry.type == TIFF_TYPE_RATIONAL && entry.count)
            metadata.exposureTime = readRational(data, entry, 0, little);
        else if (entry.tag == TIFF_TAG_ISO && entry.count) {
            metadata.iso = entry.type == TIFF_TYPE_SHORT
                ? read16(data.data() + entry.valueOffset, little)
                : read32(data.data() + entry.valueOffset, little);
        } else if (entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
                   entry.type == TIFF_TYPE_SRATIONAL && entry.count) {
            metadata.baselineExposure = readRational(data, entry, 0, little);
            metadata.hasBaselineExposure = true;
        } else if (entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                   entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3) {
            for (uint32_t c = 0; c < 3; ++c)
                metadata.asShotNeutral[c] = static_cast<float>(readRational(data, entry, c, little));
            metadata.hasAsShotNeutral = true;
        }
    }
    metadata.hasExposure = metadata.iso > 0.0 && metadata.exposureTime > 0.0;
    return true;
}

bool DNGDecoder::updateMetadata(std::vector<uint8_t>& data,
                                const double* baselineExposure,
                                const std::array<float, 3>* asShotNeutral) {
    bool little = true;
    auto entries = findTiffEntries(data, little);
    bool hasBaseline = false;
    bool hasNeutral = false;
    for (const auto& entry : entries) {
        hasBaseline |= entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
                       entry.type == TIFF_TYPE_SRATIONAL && entry.count;
        hasNeutral |= entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                      entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3;
    }
    if (!addMissingMetadataEntries(data, baselineExposure && !hasBaseline,
                                   asShotNeutral && !hasNeutral, little))
        return false;
    if ((baselineExposure && !hasBaseline) || (asShotNeutral && !hasNeutral))
        entries = findTiffEntries(data, little);
    bool baselineWritten = baselineExposure == nullptr;
    bool neutralWritten = asShotNeutral == nullptr;
    for (const auto& entry : entries) {
        if (!baselineWritten && entry.tag == TIFF_TAG_BASELINE_EXPOSURE &&
            entry.type == TIFF_TYPE_SRATIONAL && entry.count) {
            writeRational(data, entry, 0, *baselineExposure, little);
            baselineWritten = true;
        } else if (!neutralWritten && entry.tag == TIFF_TAG_AS_SHOT_NEUTRAL &&
                   entry.type == TIFF_TYPE_RATIONAL && entry.count >= 3) {
            for (uint32_t c = 0; c < 3; ++c)
                writeRational(data, entry, c, (*asShotNeutral)[c], little);
            neutralWritten = true;
        }
    }
    return baselineWritten && neutralWritten;
}

bool DNGDecoder::bakeGainMaps(std::vector<uint8_t>& data,
                              bool normalizeGainMaps,
                              bool colorOnly) {
    bool little = true;
    const auto entries = findTiffEntries(data, little);
    if (entries.empty()) return false;
    auto scalar = [&](const TiffEntry& e, uint32_t index = 0) -> uint32_t {
        const size_t pos = e.valueOffset + index * (e.type == TIFF_TYPE_SHORT ? 2 : 4);
        return e.type == TIFF_TYPE_SHORT ? read16(data.data() + pos, little)
                                         : read32(data.data() + pos, little);
    };
    const TiffEntry* photo = nullptr;
    for (const auto& e : entries)
        if (e.tag == TIFF_TAG_PHOTOMETRIC && scalar(e) == TIFF_PHOTOMETRIC_CFA) {
            photo = &e;
            break;
        }
    if (!photo) return false;
    const uint32_t rawIfd = photo->ifdOffset;
    auto find = [&](uint16_t tag) -> const TiffEntry* {
        for (const auto& e : entries) if (e.ifdOffset == rawIfd && e.tag == tag) return &e;
        return nullptr;
    };
    const auto widthE = find(TIFF_TAG_IMAGE_WIDTH), heightE = find(TIFF_TAG_IMAGE_HEIGHT);
    const auto bitsE = find(TIFF_TAG_BITS_PER_SAMPLE), compressionE = find(TIFF_TAG_COMPRESSION);
    const auto offsetsE = find(TIFF_TAG_STRIP_OFFSETS), countsE = find(TIFF_TAG_STRIP_BYTE_COUNTS);
    const auto opcodeE = find(TIFF_TAG_OPCODE_LIST_2);
    if (!widthE || !heightE || !bitsE || !compressionE || !offsetsE || !countsE ||
        !opcodeE || offsetsE->count != 1 || countsE->count != 1)
        return false;
    if (find(TIFF_TAG_LINEARIZATION_TABLE)) return false;
    const uint32_t width = scalar(*widthE), height = scalar(*heightE);
    const uint32_t bits = scalar(*bitsE), compression = scalar(*compressionE);
    const uint32_t stripOffset = scalar(*offsetsE), stripBytes = scalar(*countsE);
    if (!width || !height || bits < 8 || bits > 16 || stripOffset > data.size() ||
        stripBytes > data.size() - stripOffset) return false;

    std::vector<GainMap> maps;
    if (!parseOpcodeGainMaps(data.data() + opcodeE->valueOffset, opcodeE->count, maps))
        return false;
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
    if (compression == TIFF_COMPRESSION_JPEG) {
        lj92 decoder = nullptr;
        int decodedWidth = 0, decodedHeight = 0, decodedBits = 0, components = 0;
        if (lj92_open(&decoder, data.data() + stripOffset, stripBytes, &decodedWidth,
                      &decodedHeight, &decodedBits, &components) != LJ92_ERROR_NONE)
            return false;
        const bool valid = decodedWidth == static_cast<int>(width) &&
                           decodedHeight == static_cast<int>(height) && components == 1 &&
                           lj92_decode(decoder, pixels.data(), width, 0, nullptr, 0) == LJ92_ERROR_NONE;
        lj92_close(decoder);
        if (!valid) return false;
    } else if (compression == TIFF_COMPRESSION_NONE) {
        const size_t rowBytes = (static_cast<size_t>(width) * bits + 7) / 8;
        if (rowBytes * height > stripBytes) return false;
        for (uint32_t y = 0; y < height; ++y) {
            size_t bitOffset = static_cast<size_t>(y) * rowBytes * 8;
            for (uint32_t x = 0; x < width; ++x) {
                uint16_t value = 0;
                for (uint32_t b = 0; b < bits; ++b, ++bitOffset)
                    value = static_cast<uint16_t>((value << 1) |
                        ((data[stripOffset + bitOffset / 8] >> (7 - bitOffset % 8)) & 1));
                pixels[static_cast<size_t>(y) * width + x] = value;
            }
        }
    } else return false;

    auto mapGain = [&](const GainMap& map, uint32_t x, uint32_t y) {
        if (x < map.left || x >= map.right || y < map.top || y >= map.bottom ||
            !map.rowPitch || !map.colPitch || (y - map.top) % map.rowPitch ||
            (x - map.left) % map.colPitch) return 1.0f;
        const double nx = static_cast<double>(x) / std::max(1u, width);
        const double ny = static_cast<double>(y) / std::max(1u, height);
        const double gx = map.spacingH > 0 ? (nx - map.originH) / map.spacingH : 0;
        const double gy = map.spacingV > 0 ? (ny - map.originV) / map.spacingV : 0;
        const size_t x0 = std::min<size_t>(map.width - 1, static_cast<size_t>(std::max(0.0, std::floor(gx))));
        const size_t y0 = std::min<size_t>(map.height - 1, static_cast<size_t>(std::max(0.0, std::floor(gy))));
        const size_t x1 = std::min<size_t>(map.width - 1, x0 + 1), y1 = std::min<size_t>(map.height - 1, y0 + 1);
        const float fx = static_cast<float>(std::clamp(gx - std::floor(gx), 0.0, 1.0));
        const float fy = static_cast<float>(std::clamp(gy - std::floor(gy), 0.0, 1.0));
        const size_t channel = map.channels >= 4
            ? std::min<size_t>(map.channels - 1, (y & 1u) * 2 + (x & 1u))
            : 0;
        auto at = [&](size_t xx, size_t yy) { return map.data[(yy * map.width + xx) * map.channels + channel]; };
        return (at(x0,y0) * (1-fx) + at(x1,y0) * fx) * (1-fy) +
               (at(x0,y1) * (1-fx) + at(x1,y1) * fx) * fy;
    };
    float maximumGain = 1.0f;
    for (const auto& map : maps)
        for (float gain : map.data) if (std::isfinite(gain)) maximumGain = std::max(maximumGain, gain);
    const auto blackE = find(TIFF_TAG_BLACK_LEVEL), whiteE = find(TIFF_TAG_WHITE_LEVEL);
    double black = 0.0, white = static_cast<double>((1u << std::min(16u, bits)) - 1);
    if (blackE && blackE->count) black = blackE->type == TIFF_TYPE_RATIONAL
        ? readRational(data, *blackE, 0, little) : scalar(*blackE);
    if (whiteE && whiteE->count) white = whiteE->type == TIFF_TYPE_RATIONAL
        ? readRational(data, *whiteE, 0, little) : scalar(*whiteE);
    const double destinationWhite = normalizeGainMaps ? white : 65535.0;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        float gain = 1.0f;
        for (const auto& map : maps) gain *= mapGain(map, x, y);
        if (colorOnly) {
            float localMinimum = gain;
            for (uint32_t phaseY = 0; phaseY < 2; ++phaseY)
                for (uint32_t phaseX = 0; phaseX < 2; ++phaseX) {
                    float phaseGain = 1.0f;
                    const uint32_t px = std::min(width - 1, (x & ~1u) + phaseX);
                    const uint32_t py = std::min(height - 1, (y & ~1u) + phaseY);
                    for (const auto& map : maps) phaseGain *= mapGain(map, px, py);
                    localMinimum = std::min(localMinimum, phaseGain);
                }
            if (localMinimum > 0) gain /= localMinimum;
        }
        if (normalizeGainMaps) gain /= maximumGain;
        const size_t index = static_cast<size_t>(y) * width + x;
        const double linear = std::max(0.0, (pixels[index] - black) / std::max(1.0, white - black));
        pixels[index] = static_cast<uint16_t>(std::clamp(std::lround(linear * gain * destinationWhite), 0l, 65535l));
    }

    if (data.size() & 1u) data.push_back(0);
    const uint32_t newOffset = static_cast<uint32_t>(data.size());
    const uint32_t newBytes = width * height * 2;
    data.resize(data.size() + newBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (little) { data[newOffset + i*2] = pixels[i] & 0xff; data[newOffset + i*2+1] = pixels[i] >> 8; }
        else { data[newOffset + i*2] = pixels[i] >> 8; data[newOffset + i*2+1] = pixels[i] & 0xff; }
    }
    auto writeScalar = [&](const TiffEntry& e, uint32_t value, uint32_t index = 0) {
        const size_t pos = e.valueOffset + index * (e.type == TIFF_TYPE_SHORT ? 2 : 4);
        if (e.type == TIFF_TYPE_SHORT) write16(data.data() + pos, static_cast<uint16_t>(value), little);
        else write32(data.data() + pos, value, little);
    };
    writeScalar(*bitsE, 16); writeScalar(*compressionE, TIFF_COMPRESSION_NONE);
    writeScalar(*offsetsE, newOffset); writeScalar(*countsE, newBytes);
    if (whiteE) writeScalar(*whiteE, static_cast<uint32_t>(destinationWhite));
    if (blackE) {
        if (blackE->type == TIFF_TYPE_RATIONAL)
            for (uint32_t i = 0; i < blackE->count; ++i) writeRational(data, *blackE, i, 0.0, little);
        else for (uint32_t i = 0; i < blackE->count; ++i) writeScalar(*blackE, 0, i);
    }
    // Keep the valid OpcodeList2 tag, but make its list empty to prevent a second application.
    write32(data.data() + opcodeE->valueOffset, 0, false);
    return true;
}

bool DNGDecoder::readDNGFile(const std::string& filePath, std::vector<uint8_t>& data) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("Could not open DNG file: {}", filePath);
        return false;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read entire file
    data.resize(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    
    if (!file.good()) {
        spdlog::error("Error reading DNG file: {}", filePath);
        return false;
    }
    
    spdlog::debug("DNGDecoder: Read DNG file {} ({} bytes)", filePath, fileSize);
    return true;
}

bool DNGDecoder::readDNGGainMap(const std::string& dngPath, GainMap& gainMap) {
    std::vector<uint8_t> dngData;
    if (!readDNGFile(dngPath, dngData)) {
        return false;
    }
    
    bool little = true;
    std::vector<GainMap> maps;
    for (const auto& entry : findTiffEntries(dngData, little)) {
        if (entry.tag == TIFF_TAG_OPCODE_LIST_2 && entry.count &&
            parseOpcodeGainMaps(dngData.data() + entry.valueOffset, entry.count, maps) &&
            !maps.empty()) {
            gainMap = maps.front();
            return true;
        }
    }
    return false;
}

bool DNGDecoder::parseOpcodeGainMaps(const uint8_t* opcodeData, size_t opcodeSize,
                                    std::vector<GainMap>& gainMaps) {
    if (opcodeSize < 4) return false;
    const uint32_t numOpcodes = readBE32(opcodeData);
    size_t offset = 4;
    for (uint32_t i = 0; i < numOpcodes; ++i) {
        if (offset + 16 > opcodeSize) return false;
        const uint32_t id = readBE32(opcodeData + offset);
        const uint32_t bytes = readBE32(opcodeData + offset + 12);
        offset += 16;
        if (bytes > opcodeSize - offset) return false;
        if (id == OPCODE_GAIN_MAP && bytes >= 76) {
            const uint8_t* p = opcodeData + offset;
            GainMap map{};
            map.top = readBE32(p); map.left = readBE32(p + 4);
            map.bottom = readBE32(p + 8); map.right = readBE32(p + 12);
            map.plane = readBE32(p + 16); map.planes = readBE32(p + 20);
            map.rowPitch = readBE32(p + 24); map.colPitch = readBE32(p + 28);
            map.height = readBE32(p + 32); map.width = readBE32(p + 36);
            map.spacingV = readBEDouble(p + 40); map.spacingH = readBEDouble(p + 48);
            map.originV = readBEDouble(p + 56); map.originH = readBEDouble(p + 64);
            map.channels = readBE32(p + 72);
            const uint64_t samples = static_cast<uint64_t>(map.width) * map.height * map.channels;
            if (!map.width || !map.height || !map.channels || samples > (bytes - 76) / 4)
                return false;
            map.data.resize(static_cast<size_t>(samples));
            for (size_t sample = 0; sample < map.data.size(); ++sample)
                map.data[sample] = readBEFloat(p + 76 + sample * 4);
            gainMaps.push_back(std::move(map));
        }
        offset += bytes;
    }
    return !gainMaps.empty();
}

} // namespace motioncam

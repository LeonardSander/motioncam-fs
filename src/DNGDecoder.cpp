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

    uint16_t read16(const uint8_t* p, bool little) {
        return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                      : static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
    uint32_t read32(const uint8_t* p, bool little) {
        return little
            ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
            : static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
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
                    result.push_back({tag, type, itemCount, valueOffset});
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
    
    if (dngData.size() < 8) {
        return false;
    }
    
    // Check TIFF header
    bool littleEndian = false;
    uint16_t byteOrder = *reinterpret_cast<const uint16_t*>(dngData.data());
    
    if (byteOrder == TIFF_LITTLE_ENDIAN) {
        littleEndian = true;
    } else if (byteOrder != TIFF_BIG_ENDIAN) {
        spdlog::debug("Invalid TIFF header in DNG: {}", dngPath);
        return false;
    }
    
    // Read TIFF magic number
    uint16_t magic = *reinterpret_cast<const uint16_t*>(dngData.data() + 2);
    uint16_t expectedMagic = littleEndian ? magic : __builtin_bswap16(magic);
    if (expectedMagic != TIFF_MAGIC) {
        spdlog::debug("Invalid TIFF magic in DNG: {}", dngPath);
        return false;
    }
    
    // Read first IFD offset
    uint32_t ifdOffset = *reinterpret_cast<const uint32_t*>(dngData.data() + 4);
    if (!littleEndian) {
        ifdOffset = __builtin_bswap32(ifdOffset);
    }
    
    // Parse IFD to find opcode lists
    while (ifdOffset != 0 && ifdOffset < dngData.size()) {
        if (ifdOffset + 2 > dngData.size()) break;
        
        uint16_t numEntries = *reinterpret_cast<const uint16_t*>(dngData.data() + ifdOffset);
        if (!littleEndian) {
            numEntries = __builtin_bswap16(numEntries);
        }
        
        size_t entryOffset = ifdOffset + 2;
        
        for (uint16_t i = 0; i < numEntries && entryOffset + 12 <= dngData.size(); ++i) {
            const uint8_t* entry = dngData.data() + entryOffset;
            
            uint16_t tag = *reinterpret_cast<const uint16_t*>(entry);
            uint32_t count = *reinterpret_cast<const uint32_t*>(entry + 4);
            uint32_t valueOffset = *reinterpret_cast<const uint32_t*>(entry + 8);
            
            if (!littleEndian) {
                tag = __builtin_bswap16(tag);
                count = __builtin_bswap32(count);
                valueOffset = __builtin_bswap32(valueOffset);
            }
            
            // Check for opcode lists
            if (tag == TIFF_TAG_OPCODE_LIST_2 || tag == TIFF_TAG_OPCODE_LIST_3) {
                if (valueOffset < dngData.size() && valueOffset + count <= dngData.size()) {
                    if (parseOpcodeGainMap(dngData.data() + valueOffset, count, gainMap)) {
                        spdlog::debug("Found gain map in DNG: {}", dngPath);
                        return true;
                    }
                }
            }
            
            entryOffset += 12;
        }
        
        // Read next IFD offset
        if (entryOffset + 4 <= dngData.size()) {
            ifdOffset = *reinterpret_cast<const uint32_t*>(dngData.data() + entryOffset);
            if (!littleEndian) {
                ifdOffset = __builtin_bswap32(ifdOffset);
            }
        } else {
            break;
        }
    }
    
    return false;
}

bool DNGDecoder::parseOpcodeGainMap(const uint8_t* opcodeData, size_t opcodeSize, GainMap& gainMap) {
    if (opcodeSize < 4) return false;
    
    // Read number of opcodes
    uint32_t numOpcodes = *reinterpret_cast<const uint32_t*>(opcodeData);
    // Assume little endian for now - should check TIFF header
    
    size_t offset = 4;
    
    for (uint32_t i = 0; i < numOpcodes && offset < opcodeSize; ++i) {
        if (offset + 8 > opcodeSize) break;
        
        uint32_t opcodeId = *reinterpret_cast<const uint32_t*>(opcodeData + offset);
        uint32_t opcodeSize = *reinterpret_cast<const uint32_t*>(opcodeData + offset + 4);
        
        offset += 8;
        
        if (opcodeId == OPCODE_GAIN_MAP && offset + opcodeSize <= opcodeSize) {
            // Parse gain map opcode
            if (opcodeSize >= 24) {
                const uint8_t* gainMapData = opcodeData + offset;
                
                gainMap.top = *reinterpret_cast<const float*>(gainMapData);
                gainMap.left = *reinterpret_cast<const float*>(gainMapData + 4);
                gainMap.bottom = *reinterpret_cast<const float*>(gainMapData + 8);
                gainMap.right = *reinterpret_cast<const float*>(gainMapData + 12);
                gainMap.width = *reinterpret_cast<const uint32_t*>(gainMapData + 16);
                gainMap.height = *reinterpret_cast<const uint32_t*>(gainMapData + 20);
                
                // Read gain map data
                size_t dataSize = gainMap.width * gainMap.height * sizeof(float);
                if (opcodeSize >= 24 + dataSize) {
                    gainMap.data.resize(gainMap.width * gainMap.height);
                    std::memcpy(gainMap.data.data(), gainMapData + 24, dataSize);
                    gainMap.channels = 1; // Assuming single channel for now
                    
                    spdlog::debug("Parsed gain map: {}x{}, bounds: {},{} to {},{}", 
                                  gainMap.width, gainMap.height, 
                                  gainMap.left, gainMap.top, gainMap.right, gainMap.bottom);
                    return true;
                }
            }
        }
        
        offset += opcodeSize;
    }
    
    return false;
}

} // namespace motioncam

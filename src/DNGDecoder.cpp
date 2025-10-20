#include "DNGDecoder.h"
#include <spdlog/spdlog.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <algorithm>
#include <cstring>

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

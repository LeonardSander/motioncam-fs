#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "liblj92/lj92.h"
#include "DNGDecoder.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

int main() {
    constexpr unsigned int width = 32;
    constexpr unsigned int height = 24;
    constexpr unsigned short bits = 12;

    std::vector<uint16_t> pixels(width * height);
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            pixels[y * width + x] =
                static_cast<uint16_t>((x * 31 + y * 17) & 0x0fff);
        }
    }

    tinydngwriter::DNGImage image;
    image.SetBigEndian(false);
    assert(image.SetImageWidth(width));
    assert(image.SetImageLength(height));
    assert(image.SetRowsPerStrip(height));
    assert(image.SetSamplesPerPixel(1));
    assert(image.SetBitsPerSample(1, &bits));
    assert(image.SetCompression(tinydngwriter::COMPRESSION_JPEG));
    assert(image.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA));
    assert(image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG));
    assert(image.SetWhiteLevel(0x0fff));
    assert(image.SetImageData(
        reinterpret_cast<const unsigned char*>(pixels.data()),
        pixels.size() * sizeof(uint16_t)));
    assert(image.GetStripBytes() > 0);
    assert(image.GetStripBytes() < pixels.size() * sizeof(uint16_t));

    tinydngwriter::DNGWriter writer(false);
    assert(writer.AddImage(&image));
    std::ostringstream output(std::ios::binary);
    std::string error;
    assert(writer.WriteToFile(output, &error));
    assert(error.empty());
    assert(output.str().size() > image.GetStripBytes());

    std::string dng = output.str();
    const char jpegMarker[] = {
        static_cast<char>(0xff), static_cast<char>(0xd8)};
    auto jpegStart = std::search(
        dng.begin(), dng.end(), std::begin(jpegMarker), std::end(jpegMarker));
    assert(jpegStart != dng.end());

    lj92 decoder = nullptr;
    int decodedWidth = 0;
    int decodedHeight = 0;
    int decodedBits = 0;
    int decodedComponents = 0;
    auto* encodedData = reinterpret_cast<uint8_t*>(&*jpegStart);
    int encodedSize = static_cast<int>(dng.end() - jpegStart);
    assert(lj92_open(
        &decoder,
        encodedData,
        encodedSize,
        &decodedWidth,
        &decodedHeight,
        &decodedBits,
        &decodedComponents) == LJ92_ERROR_NONE);
    assert(decodedWidth == static_cast<int>(width));
    assert(decodedHeight == static_cast<int>(height));
    assert(decodedBits == bits);
    assert(decodedComponents == 1);

    std::vector<uint16_t> decoded(pixels.size());
    assert(lj92_decode(
        decoder, decoded.data(), decodedWidth, 0, nullptr, 0) ==
        LJ92_ERROR_NONE);
    lj92_close(decoder);
    assert(decoded == pixels);

    constexpr unsigned int rgbWidth = 19;
    constexpr unsigned int rgbHeight = 13;
    constexpr int rgbComponents = 3;
    std::vector<uint16_t> rgb(rgbWidth * rgbHeight * rgbComponents);
    for (unsigned int y = 0; y < rgbHeight; ++y) {
        for (unsigned int x = 0; x < rgbWidth; ++x) {
            const auto offset = (y * rgbWidth + x) * rgbComponents;
            rgb[offset + 0] = static_cast<uint16_t>((x * 37 + y * 11) & 0x0fff);
            rgb[offset + 1] = static_cast<uint16_t>((x * 13 + y * 43 + 700) & 0x0fff);
            rgb[offset + 2] = static_cast<uint16_t>((x * 29 + y * 7 + 1400) & 0x0fff);
        }
    }

    uint8_t* rgbEncoded = nullptr;
    int rgbEncodedSize = 0;
    assert(lj92_encode(
        rgb.data(), rgbWidth, rgbHeight, bits, rgbComponents,
        rgbWidth * rgbComponents, 0, nullptr, 0,
        &rgbEncoded, &rgbEncodedSize) == LJ92_ERROR_NONE);
    assert(rgbEncoded != nullptr);
    assert(rgbEncodedSize > 0);

    decoder = nullptr;
    assert(lj92_open(
        &decoder, rgbEncoded, rgbEncodedSize,
        &decodedWidth, &decodedHeight, &decodedBits,
        &decodedComponents) == LJ92_ERROR_NONE);
    assert(decodedWidth == static_cast<int>(rgbWidth));
    assert(decodedHeight == static_cast<int>(rgbHeight));
    assert(decodedBits == bits);
    assert(decodedComponents == rgbComponents);

    std::vector<uint16_t> decodedRgb(rgb.size());
    assert(lj92_decode(
        decoder, decodedRgb.data(), decodedWidth * rgbComponents,
        0, nullptr, 0) == LJ92_ERROR_NONE);
    lj92_close(decoder);
    free(rgbEncoded);
    assert(decodedRgb == rgb);

    // Alternating mosaiced data can be strongly correlated diagonally. The
    // former hard-coded JPEG predictor 6 compares unlike neighboring samples
    // and compresses this kind of data poorly; select diagonal predictor 3.
    constexpr int cfaWidth = 256;
    constexpr int cfaHeight = 192;
    std::vector<uint16_t> cfa(cfaWidth * cfaHeight);
    for (int y = 0; y < cfaHeight; ++y)
        for (int x = 0; x < cfaWidth; ++x)
            cfa[y * cfaWidth + x] = ((x + y) & 1) ? 3000 : 600;
    uint8_t* cfaEncoded = nullptr;
    int cfaEncodedSize = 0;
    assert(lj92_encode(cfa.data(), cfaWidth, cfaHeight, bits, 1,
                      cfaWidth, 0, nullptr, 0,
                      &cfaEncoded, &cfaEncodedSize) == LJ92_ERROR_NONE);
    assert(cfaEncodedSize < static_cast<int>(cfa.size() * sizeof(uint16_t) / 8));
    const std::array<uint8_t, 2> startOfScan = {0xff, 0xda};
    const auto sos = std::search(cfaEncoded, cfaEncoded + cfaEncodedSize,
                                 startOfScan.begin(), startOfScan.end());
    assert(sos != cfaEncoded + cfaEncodedSize);
    const int componentsInScan = sos[4];
    assert(sos[5 + componentsInScan * 2] == 3);
    free(cfaEncoded);

    tinydngwriter::GainMapParams gainMap{};
    gainMap.top = 0; gainMap.left = 0; gainMap.bottom = height; gainMap.right = width;
    gainMap.plane = 0; gainMap.planes = 1;
    gainMap.row_pitch = 1; gainMap.col_pitch = 1;
    gainMap.map_points_v = 2; gainMap.map_points_h = 2;
    gainMap.map_spacing_v = 1.0; gainMap.map_spacing_h = 1.0;
    gainMap.map_origin_v = 0.0; gainMap.map_origin_h = 0.0;
    gainMap.map_planes = 4;
    gainMap.gain_data.resize(16);
    for (size_t plane = 0; plane < 4; ++plane)
        for (size_t point = 0; point < 4; ++point)
            gainMap.gain_data[plane * 4 + point] = 1.0f + 0.1f * plane + 0.05f * point;
    tinydngwriter::OpcodeList opcodes;
    opcodes.AddGainMap(gainMap);
    assert(image.SetOpcodeList2(opcodes));
    std::ostringstream gainMapOutput(std::ios::binary);
    assert(writer.WriteToFile(gainMapOutput, &error));
    const std::string gainMapDng = gainMapOutput.str();
    std::vector<uint8_t> baked(gainMapDng.begin(), gainMapDng.end());
    const size_t originalSize = baked.size();
    assert(motioncam::DNGDecoder::bakeGainMaps(baked, false, false));
    assert(baked.size() > originalSize);

    std::vector<uint8_t> colorBaked(gainMapDng.begin(), gainMapDng.end());
    assert(motioncam::DNGDecoder::bakeGainMaps(colorBaked, false, true));
    // OpcodeList2 (51009 / 0xc741) is replaced by OpcodeList3
    // (51022 / 0xc74e) in this little-endian test DNG.
    const std::array<uint8_t, 2> opcodeList3Tag = {0x4e, 0xc7};
    assert(std::search(colorBaked.begin(), colorBaked.end(),
                       opcodeList3Tag.begin(), opcodeList3Tag.end()) != colorBaked.end());

    // Android-style DNGs commonly store the four CFA phases as four separate
    // one-channel GainMap opcodes. All of them must be transformed in place.
    tinydngwriter::OpcodeList fourGainOpcodes;
    for (unsigned int phase = 0; phase < 4; ++phase) {
        tinydngwriter::GainMapParams phaseMap = gainMap;
        phaseMap.top = phase / 2;
        phaseMap.left = phase % 2;
        phaseMap.row_pitch = 2;
        phaseMap.col_pitch = 2;
        phaseMap.map_planes = 1;
        phaseMap.gain_data.resize(4);
        for (size_t point = 0; point < phaseMap.gain_data.size(); ++point)
            phaseMap.gain_data[point] = 1.0f + 0.1f * phase + 0.05f * point;
        fourGainOpcodes.AddGainMap(phaseMap);
    }
    assert(image.SetOpcodeList2(fourGainOpcodes));
    std::ostringstream fourGainOutput(std::ios::binary);
    assert(writer.WriteToFile(fourGainOutput, &error));
    const std::string fourGainDng = fourGainOutput.str();
    std::vector<uint8_t> fourGainBytes(fourGainDng.begin(), fourGainDng.end());
    assert(motioncam::DNGDecoder::transformGainMaps(fourGainBytes, false, true, false));

    return 0;
}

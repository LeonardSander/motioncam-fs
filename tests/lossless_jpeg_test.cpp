#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tinydng/tiny_dng_writer.h"
#include "liblj92/lj92.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
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

    return 0;
}

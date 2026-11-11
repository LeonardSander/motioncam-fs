#include "GyroTelemetry.h"
#include "DNGDecoder.h"
#include <cmath>
#include <limits>

namespace rawpreamp {
bool GyroTelemetry::fillMissingDngMetadata(std::vector<uint8_t>& bytes, size_t index) const {
    if (mFormat != Format::Jsonl || index >= mFrames.size()) return true;
    const auto& capture = mFrames[index].message.at("frame");
    std::vector<DNGSidecarMetadataEntry> tags;
    const auto ascii = [&](uint16_t tag, const std::string& text, bool exif = false) {
        if (text.empty()) return;
        DNGSidecarMetadataEntry entry;
        entry.tag = tag;
        entry.type = 2;
        entry.count = text.size() + 1;
        entry.exif = exif;
        entry.value.assign(text.begin(), text.end());
        entry.value.push_back(0);
        tags.push_back(std::move(entry));
    };
    const auto rational = [&](uint16_t tag, double value) {
        if (!std::isfinite(value) || value <= 0) return;
        uint32_t denominator = 1000000;
        while (value * denominator > std::numeric_limits<uint32_t>::max() && denominator > 1)
            denominator /= 10;
        if (value * denominator > std::numeric_limits<uint32_t>::max()) return;
        const auto numerator = static_cast<uint32_t>(std::llround(value * denominator));
        if (!numerator) return;
        DNGSidecarMetadataEntry entry;
        entry.tag = tag;
        entry.type = 5;
        entry.count = 1;
        entry.exif = true;
        for (const auto number : {numerator, denominator})
            for (unsigned i = 0; i < 4; ++i) entry.value.push_back(static_cast<uint8_t>(number >> (i * 8)));
        tags.push_back(std::move(entry));
    };
    if (mHeader.contains("camera")) {
        const auto& camera = mHeader["camera"];
        ascii(271, camera.value("cameraBrand", ""));
        ascii(272, camera.value("cameraModel", ""));
        const std::string brand = camera.value("cameraBrand", "");
        const std::string model = camera.value("cameraModel", "");
        ascii(50708, brand.empty() ? model : model.empty() ? brand : brand + " " + model);
        ascii(42035, camera.value("lensBrand", ""), true);
        ascii(42036, camera.value("lensModel", ""), true);
    }
    rational(33434, capture.value("exposureTimeUs", 0.0) / 1e6);
    const auto iso = capture.value("iso", 0u);
    if (iso && iso <= 65535) {
        DNGSidecarMetadataEntry entry;
        entry.tag = 34855;
        entry.type = 3;
        entry.count = 1;
        entry.exif = true;
        entry.value = {static_cast<uint8_t>(iso), static_cast<uint8_t>(iso >> 8)};
        tags.push_back(std::move(entry));
    }
    if (capture.contains("lens") && !capture["lens"].empty()) {
        const auto& lens = capture["lens"][0];
        rational(33437, lens.value("fNumber", 0.0));
        rational(37386, lens.value("focalLengthMm", 0.0));
        rational(37382, lens.value("focusDistanceMm", 0.0) / 1000.0);
    }
    return tags.empty() || DNGDecoder::fillMissingSidecarMetadata(bytes, tags);
}
} // namespace rawpreamp

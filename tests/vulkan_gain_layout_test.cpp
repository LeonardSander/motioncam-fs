#include "VulkanGainBake.h"
#include "VulkanGalleryPreview.h"
#include "Utils.h"
#include "GalleryColor.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/gpu.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
    // Compare the gallery compute pass with the established CPU fallback,
    // including odd dimensions and all four Bayer phases.
    for (const std::array<uint8_t, 4> phase : {
             std::array<uint8_t, 4>{0, 1, 1, 2},
             std::array<uint8_t, 4>{2, 1, 1, 0},
             std::array<uint8_t, 4>{1, 0, 2, 1},
             std::array<uint8_t, 4>{1, 2, 0, 1}}) {
        constexpr uint32_t previewWidth = 33, previewHeight = 31;
        std::vector<uint16_t> mosaic(previewWidth * previewHeight);
        for (size_t index = 0; index < mosaic.size(); ++index)
            mosaic[index] = static_cast<uint16_t>((index * 719u + index / 7u) % 16384u);
        const std::array<double, 3> black{64.0, 72.0, 80.0};
        const std::array<double, 3> white{16000.0, 16000.0, 16000.0};
        std::vector<uint8_t> gpu, cpu;
        if (!motioncam::demosaicGalleryBayerVulkan(
                mosaic.data(), gpu, previewWidth, previewHeight,
                phase, black, white)) {
            std::fprintf(stderr, "Vulkan gallery demosaic unavailable; test skipped\n");
            return 77;
        }
        if (!motioncam::utils::demosaicBilinearBayerToRgb16Bytes(
                mosaic.data(), cpu, previewWidth, previewHeight,
                phase, black, white) || gpu.size() != cpu.size()) {
            std::fprintf(stderr, "Gallery demosaic output size mismatch\n");
            return 1;
        }
        for (size_t index = 0; index < gpu.size() / 2; ++index) {
            uint16_t actual, expected;
            std::memcpy(&actual, gpu.data() + index * 2, 2);
            std::memcpy(&expected, cpu.data() + index * 2, 2);
            if (std::abs(int(actual) - int(expected)) > 1) {
                std::fprintf(stderr, "Gallery demosaic mismatch at %zu: %u versus %u\n",
                             index, actual, expected);
                return 1;
            }
        }
        motioncam::VulkanGalleryBayerSource source;
        source.samples = mosaic.data();
        source.width = previewWidth;
        source.height = previewHeight;
        source.phase = phase;
        source.black = black;
        source.white = white;
        const std::array<float, 3> gains{1.15f, 0.95f, 1.25f};
        const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
        auto separate = cpu;
        std::vector<uint8_t> fused;
        if (!motioncam::colorGalleryRgbVulkan(separate, gains, identity,
                                               identity, 1.0f, nullptr, nullptr,
                                               false, false) ||
            !motioncam::colorGalleryRgbVulkan(fused, gains, identity,
                                              identity, 1.0f, nullptr, nullptr,
                                              false, false, &source) ||
            fused.size() != separate.size()) {
            std::fprintf(stderr, "Fused gallery Bayer color pass failed\n");
            return 1;
        }
        for (size_t index = 0; index < fused.size() / 2; ++index) {
            uint16_t actual, expected;
            std::memcpy(&actual, fused.data() + index * 2, 2);
            std::memcpy(&expected, separate.data() + index * 2, 2);
            if (std::abs(int(actual) - int(expected)) > 4) {
                std::fprintf(stderr, "Fused Bayer color mismatch at %zu: %u vs %u\n", index, actual, expected);
                return 1;
            }
        }
        // Strict shared-image validation must reject host input instead of
        // silently uploading a full frame and calling it zero-copy.
        motioncam::VulkanGalleryOutput strictOutput;
        strictOutput.width = previewWidth;
        strictOutput.height = previewHeight;
        strictOutput.rgb24 = true;
        strictOutput.requireZeroCopy = true;
        auto strictInput = cpu;
        if (motioncam::colorGalleryRgbVulkan(strictInput, gains, identity,
                identity, 1.0f, nullptr, nullptr, false, false, nullptr,
                &strictOutput) || strictInput != cpu) {
            std::fprintf(stderr, "Zero-copy validation accepted or modified host input\n");
            return 1;
        }
        motioncam::VulkanGalleryBayerGain bake;
        bake.scale.fill(1.0f);
        motioncam::VulkanGalleryBayerGainMap map;
        map.width = map.height = 1;
        map.channels = 4;
        map.right = previewWidth;
        map.bottom = previewHeight;
        map.spacingH = map.spacingV = 1.0f;
        map.values.assign(4, 2.0f);
        bake.maps.push_back(map);
        source.black = {0, 0, 0};
        source.white = {65535, 65535, 65535};
        source.gain = &bake;
        std::vector<uint8_t> baked;
        if (!motioncam::colorGalleryRgbVulkan(baked, gains, identity,
                identity, 1.0f, nullptr, nullptr, false, false, &source)) {
            std::fprintf(stderr, "Fused gallery gain pass failed\n");
            return 1;
        }
        std::vector<uint16_t> doubled = mosaic;
        for (auto& value : doubled) value = std::min<uint32_t>(65535, value * 2u);
        source.samples = doubled.data();
        source.gain = nullptr;
        std::vector<uint8_t> reference;
        if (!motioncam::colorGalleryRgbVulkan(reference, gains, identity,
                identity, 1.0f, nullptr, nullptr, false, false, &source) ||
            reference.size() != baked.size()) return 1;
        for (size_t index = 0; index < baked.size() / 2; ++index) {
            uint16_t actual, expected;
            std::memcpy(&actual, baked.data() + index * 2, 2);
            std::memcpy(&expected, reference.data() + index * 2, 2);
            if (std::abs(int(actual) - int(expected)) > 4) {
                std::fprintf(stderr, "Fused gallery gain mismatch at %zu: %u vs %u\n",
                             index, actual, expected);
                return 1;
            }
        }
    }
    {
        std::vector<uint8_t> pixels(257 * 6);
        const std::array<float, 3> gains{1.3f, 0.8f, 1.1f};
        const std::array<float, 9> unusedMatrix{};
        std::vector<uint16_t> source(pixels.size() / 2);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<uint16_t>((i * 7919u) & 65535u);
        source[0] = 65535;
        source[1] = 64000;
        source[2] = 65000;
        std::memcpy(pixels.data(), source.data(), pixels.size());
        if (!motioncam::colorGalleryRgbVulkan(
                pixels, gains, unusedMatrix, unusedMatrix, 1.0f,
                nullptr, nullptr, false, false)) {
            std::fprintf(stderr, "Vulkan gallery color transform unavailable\n");
            return 1;
        }
        const auto encode = [](float value) {
            const size_t index = static_cast<size_t>(
                std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
            const float linear = static_cast<float>(index) / 65535.0f;
            const float encoded = linear <= 0.0031308f ? linear * 12.92f
                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint16_t>(std::lround(
                std::clamp(encoded, 0.0f, 1.0f) * 65535.0f));
        };
        for (size_t pixel = 0; pixel < source.size() / 3; ++pixel) {
            const size_t first = pixel * 3;
            const bool clipped = source[first] == 65535 ||
                source[first + 1] == 65535 || source[first + 2] == 65535;
            const float least = static_cast<float>(std::min({source[first],
                source[first + 1], source[first + 2]})) / 65535.0f;
            const float t = std::clamp((least - 0.90f) / 0.10f, 0.0f, 1.0f);
            const float weight = t * t * (3.0f - 2.0f * t);
            std::array<float, 3> display{};
            for (size_t channel = 0; channel < 3; ++channel)
                display[channel] = source[first + channel] / 65535.0f * gains[channel];
            if (clipped)
                display[1] = std::max(display[1], std::min(display[0], display[2]));
            const float neutral = display[0] + display[1] + display[2] -
                std::min({display[0], display[1], display[2]}) -
                std::max({display[0], display[1], display[2]});
            for (size_t channel = 0; channel < 3; ++channel) {
                uint16_t actual;
                std::memcpy(&actual, pixels.data() + (first + channel) * 2, 2);
                const auto expected = encode(
                    display[channel] * (1.0f - weight) + neutral * weight);
                if (std::abs(int(actual) - int(expected)) > 2) {
                    std::fprintf(stderr, "Gallery color mismatch at %zu: %u versus %u\n",
                                 first + channel, actual, expected);
                    return 1;
                }
            }
        }
    }
    for (bool combined : {false, true}) {
        constexpr size_t count = 97;
        motioncam::DNGProfileTable hue, look;
        hue.hueDivisions = look.hueDivisions = 4;
        hue.saturationDivisions = look.saturationDivisions = 2;
        hue.valueDivisions = combined ? 1 : 2;
        look.valueDivisions = 2;
        hue.encoding = combined ? 0 : 1;
        look.encoding = 0;
        auto fill = [](motioncam::DNGProfileTable& table, float amount) {
            table.values.resize(static_cast<size_t>(table.hueDivisions) *
                table.saturationDivisions * table.valueDivisions * 3);
            for (size_t i = 0; i < table.values.size() / 3; ++i) {
                table.values[i * 3] = float(int(i % 7) - 3) * amount * 10;
                table.values[i * 3 + 1] = 1.0f + float(int(i % 5) - 2) * amount;
                table.values[i * 3 + 2] = 1.0f + float(int(i % 3) - 1) * amount;
            }
        };
        fill(hue, 0.01f);
        fill(look, 0.015f);
        std::vector<uint16_t> source(count * 3);
        for (size_t pixel = 0; pixel < count; ++pixel) {
            source[pixel * 3] = static_cast<uint16_t>(2000 + pixel * 311 % 37000);
            source[pixel * 3 + 1] = static_cast<uint16_t>(1200 + pixel * 547 % 39000);
            source[pixel * 3 + 2] = static_cast<uint16_t>(3000 + pixel * 773 % 35000);
        }
        std::vector<uint8_t> gpu(source.size() * 2);
        std::memcpy(gpu.data(), source.data(), gpu.size());
        const std::array<float, 3> gains{1.0f, 1.0f, 1.0f};
        const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
        constexpr float exposure = 1.1f;
        if (!motioncam::colorGalleryRgbVulkan(gpu, gains, identity, identity,
                                               exposure, &hue, &look, true, false)) {
            std::fprintf(stderr, "Vulkan profile color transform unavailable\n");
            return 1;
        }
        for (size_t pixel = 0; pixel < count; ++pixel) {
            std::array<float, 3> camera{
                source[pixel * 3] / 65535.0f,
                source[pixel * 3 + 1] / 65535.0f,
                source[pixel * 3 + 2] / 65535.0f};
            std::array<float, 3> proPhoto{
                1.3459433f * camera[0] - 0.2556075f * camera[1] -
                    0.0511118f * camera[2],
               -0.5445989f * camera[0] + 1.5081673f * camera[1] +
                    0.0205351f * camera[2],
                1.2118128f * camera[2]};
            if (combined)
                motioncam::gallery::applyHueSatAndLook(proPhoto, hue, look, exposure);
            else {
                motioncam::gallery::applyProfileTable(proPhoto, hue);
                for (auto& channel : proPhoto) channel *= exposure;
                motioncam::gallery::applyProfileTable(proPhoto, look);
            }
            std::array<float, 3> xyz{
                0.7976749f * proPhoto[0] + 0.1351917f * proPhoto[1] +
                    0.0313534f * proPhoto[2],
                0.2880402f * proPhoto[0] + 0.7118741f * proPhoto[1] +
                    0.0000857f * proPhoto[2],
                0.82521f * proPhoto[2]};
            std::array<float, 3> display{
                 3.1338561f * xyz[0] - 1.6168667f * xyz[1] - 0.4906146f * xyz[2],
                -0.9787684f * xyz[0] + 1.9161415f * xyz[1] + 0.0334540f * xyz[2],
                 0.0719453f * xyz[0] - 0.2289914f * xyz[1] + 1.4052427f * xyz[2]};
            for (size_t channel = 0; channel < 3; ++channel) {
                const size_t index = pixel * 3 + channel;
                const size_t tableIndex = static_cast<size_t>(
                    std::clamp(display[channel], 0.0f, 1.0f) * 65535.0f + 0.5f);
                const float linear = static_cast<float>(tableIndex) / 65535.0f;
                const float encoded = linear <= 0.0031308f ? linear * 12.92f
                    : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                const auto expected = static_cast<uint16_t>(std::lround(
                    std::clamp(encoded, 0.0f, 1.0f) * 65535.0f));
                uint16_t actual;
                std::memcpy(&actual, gpu.data() + index * 2, 2);
                if (std::abs(int(actual) - int(expected)) > 4) {
                    std::fprintf(stderr,
                        "Gallery profile mismatch combined=%d at %zu: %u versus %u\n",
                        combined, index, actual, expected);
                    return 1;
                }
            }
        }
    }
    {
        constexpr uint32_t width = 13, height = 7;
        std::vector<uint16_t> samples(width * height * 3);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<uint16_t>((i * 4001u) & 65535u);
        std::vector<uint8_t> source(samples.size() * 2);
        std::memcpy(source.data(), samples.data(), source.size());
        std::vector<uint8_t> clipping(width * height);
        for (size_t i = 0; i < clipping.size(); ++i)
            clipping[i] = i % 11 == 0 ? 1 : i % 7 == 0
                ? static_cast<uint8_t>(3 + i % 7) : 0;
        const std::array<float, 3> gains{1.0f, 1.0f, 1.0f};
        const std::array<float, 9> matrix{1,0,0,0,1,0,0,0,1};
        auto colored = source;
        if (!motioncam::colorGalleryRgbVulkan(colored, gains, matrix,
                                               matrix, 1.0f, nullptr, nullptr,
                                               false, false)) return 1;
        for (int orientation : {0, 90, 180, 270})
            for (bool rgb24 : {false, true})
              for(bool clippingOn:{true,false,true}) {
                motioncam::VulkanGalleryOutput output;
                output.width = width;
                output.height = height;
                output.orientation = orientation;
                output.rgb24 = rgb24;
                output.clipping = clippingOn?&clipping:nullptr;
                std::vector<uint8_t> thumbnail;
                if (rgb24) {
                    output.thumbnailPixels = &thumbnail;
                    output.thumbnailWidth = 5;
                    output.thumbnailHeight = 3;
                    output.thumbnailOrientation = orientation;
                }
                auto actual = source;
                if (!motioncam::colorGalleryRgbVulkan(actual, gains, matrix,
                        matrix, 1.0f, nullptr, nullptr, false, false,
                        nullptr, &output)) return 1;
                const uint32_t outputWidth = orientation == 90 || orientation == 270
                    ? height : width;
                const size_t stride = rgb24 ? 3u : 6u;
                std::vector<uint8_t> expected(static_cast<size_t>(width) * height * stride);
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x) {
                        const size_t inputPixel = static_cast<size_t>(y) * width + x;
                        uint32_t ox = x, oy = y;
                        if (orientation == 90) { ox = height - 1 - y; oy = x; }
                        else if (orientation == 180) {
                            ox = width - 1 - x; oy = height - 1 - y;
                        } else if (orientation == 270) {
                            ox = y; oy = width - 1 - x;
                        }
                        std::array<uint16_t, 3> codes{};
                        for (size_t channel = 0; channel < 3; ++channel)
                            std::memcpy(&codes[channel],
                                colored.data() + inputPixel * 6 + channel * 2, 2);
                        const uint8_t indication = clippingOn?clipping[inputPixel]:0;
                        if (indication == 1) codes = {32768, 16384, 0};
                        else if (indication > 1) {
                            const uint8_t bits = indication - 2;
                            codes = bits == 7 ? std::array<uint16_t, 3>{0, 0, 0}
                                : std::array<uint16_t, 3>{
                                    static_cast<uint16_t>(bits & 1 ? 65535 : 0),
                                    static_cast<uint16_t>(bits & 2 ? 65535 : 0),
                                    static_cast<uint16_t>(bits & 4 ? 65535 : 0)};
                        }
                        const size_t destination =
                            (static_cast<size_t>(oy) * outputWidth + ox) * stride;
                        for (size_t channel = 0; channel < 3; ++channel) {
                            if (rgb24) expected[destination + channel] =
                                static_cast<uint8_t>(codes[channel] >> 8);
                            else std::memcpy(expected.data() + destination +
                                             channel * 2, &codes[channel], 2);
                        }
                    }
                if (actual != expected) {
                    std::fprintf(stderr,
                        "Gallery output mismatch orientation=%d rgb24=%d\n",
                        orientation, rgb24);
                    return 1;
                }
                if (rgb24) {
                    std::vector<uint8_t> expectedThumbnail(5u * 3u * 3u);
                    const uint32_t fullHeight = orientation == 90 || orientation == 270
                        ? width : height;
                    for (uint32_t ty = 0; ty < 3; ++ty)
                        for (uint32_t tx = 0; tx < 5; ++tx) {
                            const uint32_t ox = std::min(outputWidth - 1,
                                (2u * tx + 1u) * outputWidth / 10u);
                            const uint32_t oy = std::min(fullHeight - 1,
                                (2u * ty + 1u) * fullHeight / 6u);
                            for(uint32_t channel=0;channel<3;++channel)
                                expectedThumbnail[(ty*5u+tx)*3u+channel]=
                                    expected[(static_cast<size_t>(oy)*outputWidth+ox)*3u+channel];
                        }
                    if (thumbnail != expectedThumbnail) {
                        std::fprintf(stderr,
                            "Gallery thumbnail mismatch orientation=%d size=%zu expected=%zu first=%u/%u\n",
                            orientation, thumbnail.size(), expectedThumbnail.size(),
                            thumbnail.empty()?0u:thumbnail[0],expectedThumbnail[0]);
                        return 1;
                    }
                }
            }
    }
    {
        // Reusing a gallery device across a seek must not deliver old pixels
        // or their frame index as a thumbnail for the new playback generation.
        const std::array<float, 3> gains{1, 1, 1};
        const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
        motioncam::VulkanGalleryOutput output;
        output.width = output.height = 16;
        output.rgb24 = true;
        output.thumbnailWidth = 5;
        output.thumbnailHeight = 3;
        std::vector<uint8_t> thumbnail;
        int completedFrame = -1;
        output.thumbnailPixels = &thumbnail;
        output.completedThumbnailSourceFrame = &completedFrame;
        std::vector<uint8_t> clipping(16u*16u,4u); // green indication
        for (uint64_t generation : {11u, 12u, 13u}) {
            output.clipping=generation==12?nullptr:&clipping;
            output.thumbnailGeneration = generation;
            output.thumbnailSourceFrame = generation == 11 ? 7 : 500;
            bool received = false;
            for (int attempt = 0; attempt < 32; ++attempt) {
                std::vector<uint16_t> samples(16u * 16u * 3u);
                const size_t channel=0u;
                for (size_t pixel = 0; pixel < 16u * 16u; ++pixel)
                    samples[pixel * 3u + channel] = 65535;
                std::vector<uint8_t> rgb(samples.size() * 2u);
                std::memcpy(rgb.data(), samples.data(), rgb.size());
                if (!motioncam::colorGalleryRgbVulkan(rgb, gains, identity,
                        identity, 1.0f, nullptr, nullptr, false, false,
                        nullptr, &output)) return 1;
                if (thumbnail.empty()) continue;
                if (completedFrame != output.thumbnailSourceFrame || thumbnail.size() != 45u) {
                    std::fprintf(stderr, "Thumbnail belongs to an old playback generation\n");
                    return 1;
                }
                for (size_t index = 0; index < thumbnail.size(); ++index) {
                    const size_t expectedChannel=generation==12?0u:1u;
                    if (thumbnail[index] != (index % 3u == expectedChannel ? 255u : 0u)) {
                        std::fprintf(stderr, "Thumbnail pixels belong to an old playback generation\n");
                        return 1;
                    }
                }
                received = true;
                // Leave another result queued before changing generations.
                if (attempt >= 3) break;
            }
            if (!received) {
                std::fprintf(stderr, "Gallery thumbnail readback did not complete\n");
                return 1;
            }
        }
    }
    {
        // A pending readback from a previous row size must be discarded, even
        // when its byte count equals the new shape's byte count.
        const std::array<float,3> gains{1,1,1};
        const std::array<float,9> identity{1,0,0,0,1,0,0,0,1};
        motioncam::VulkanGalleryOutput output;
        output.width=output.height=16;output.rgb24=true;
        output.thumbnailGeneration=77;output.thumbnailSourceFrame=19;
        std::vector<uint8_t> thumbnail;
        int completed=-1;
        output.thumbnailPixels=&thumbnail;output.completedThumbnailSourceFrame=&completed;
        for(const auto shape:{std::pair{5u,3u},std::pair{3u,5u},std::pair{4u,4u}}){
            output.thumbnailWidth=shape.first;output.thumbnailHeight=shape.second;
            bool received=false;
            for(int attempt=0;attempt<32;++attempt){
                std::vector<uint16_t> samples(16u*16u*3u);
                for(size_t i=0;i<samples.size();++i)samples[i]=uint16_t(i*7919u);
                std::vector<uint8_t> rgb(samples.size()*2u);
                std::memcpy(rgb.data(),samples.data(),rgb.size());
                if(!motioncam::colorGalleryRgbVulkan(rgb,gains,identity,identity,1.0f,
                    nullptr,nullptr,false,false,nullptr,&output))return 1;
                if(thumbnail.empty())continue;
                if(thumbnail.size()!=shape.first*shape.second*3u)return 1;
                for(uint32_t y=0;y<shape.second;++y)for(uint32_t x=0;x<shape.first;++x){
                    const uint32_t sx=(2*x+1)*16/(2*shape.first);
                    const uint32_t sy=(2*y+1)*16/(2*shape.second);
                    for(uint32_t c=0;c<3;++c)if(thumbnail[(y*shape.first+x)*3+c]!=rgb[(sy*16+sx)*3+c]){
                        std::fprintf(stderr,"Thumbnail readback used stale dimensions\n");return 1;
                    }
                }
                received=true;if(attempt>=3)break;
            }
            if(!received)return 1;
        }
    }
    {
        // RGB texture specialization must match the existing buffer shader;
        // excluding Bayer branches must not alter normalization or color math.
        pl_vulkan vk = pl_vulkan_create(nullptr, nullptr);
        if (!vk) return 1;
        const pl_fmt format = pl_find_fmt(vk->gpu, PL_FMT_UNORM, 4, 16, 16,
                                          PL_FMT_CAP_SAMPLEABLE);
        pl_tex_params params{};
        params.w = params.h = 16;
        params.format = format;
        params.sampleable = params.host_writable = true;
        pl_tex tex = format ? pl_tex_create(vk->gpu, &params) : nullptr;
        std::vector<uint16_t> rgba(16u * 16u * 4u, 65535);
        std::vector<uint16_t> samples(16u * 16u * 3u);
        for (size_t i = 0; i < samples.size(); ++i)
            rgba[(i / 3u) * 4u + i % 3u] = samples[i] = (i * 7919u) & 65535u;
        pl_tex_transfer_params transfer{};
        transfer.tex = tex;
        transfer.ptr = rgba.data();
        if (!tex || !pl_tex_upload(vk->gpu, &transfer)) return 1;
        motioncam::VulkanGalleryTextureSource texture;
        texture.gpu = vk->gpu;
        texture.texture = tex;
        texture.width = texture.height = 16;
        texture.white.fill(65535);
        const std::array<float, 3> gains{1.1f, 0.9f, 1.2f};
        const std::array<float, 9> matrix{1,0,0,0,1,0,0,0,1};
        for (bool rgb24 : {false, true}) {
            motioncam::VulkanGalleryOutput output;
            output.width = output.height = 16;
            output.orientation = 90;
            output.rgb24 = rgb24;
            std::vector<uint8_t> expected(samples.size() * 2u), actual;
            std::memcpy(expected.data(), samples.data(), expected.size());
            if (!motioncam::colorGalleryRgbVulkan(expected, gains, matrix, matrix,
                    1.0f, nullptr, nullptr, false, false, nullptr, &output) ||
                !motioncam::colorGalleryRgbVulkan(actual, gains, matrix, matrix,
                    1.0f, nullptr, nullptr, false, false, nullptr, &output, &texture) ||
                actual != expected) {
                std::fprintf(stderr, "RGB texture shader differs from buffer shader\n");
                return 1;
            }
        }
        {
            // Resident RGB clipping must match the CPU mask without source
            // readback, including thresholds, rotation and disabling it.
            texture.black = {1000, 2000, 3000};
            texture.white = {60000, 62000, 64000};
            std::vector<uint8_t> mask(256), residentMask;
            for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
                bool low = false;
                uint8_t high = 0;
                for (size_t channel = 0; channel < 3; ++channel) {
                    const auto code = samples[pixel * 3u + channel];
                    low |= code < texture.black[channel];
                    if (code >= texture.white[channel]) high |= uint8_t(1u << channel);
                }
                mask[pixel] = low ? 1u : high ? uint8_t(2u + high) : 0u;
            }
            for (int orientation : {0, 90, 180, 270}) {
                for (bool enabled : {true, false, true}) {
                    motioncam::VulkanGalleryOutput output;
                    output.width = output.height = 16;
                    output.rgb24 = true;
                    output.orientation = orientation;
                    std::vector<uint8_t> expected, actual, expectedThumbnail, actualThumbnail;
                    output.thumbnailWidth = 5;
                    output.thumbnailHeight = 3;
                    output.thumbnailPixels = &expectedThumbnail;
                    output.clipping = enabled ? &mask : nullptr;
                    if (!motioncam::colorGalleryRgbVulkan(expected, gains, matrix, matrix,
                            1.0f, nullptr, nullptr, false, false, nullptr, &output, &texture)) return 1;
                    output.clipping = enabled ? &residentMask : nullptr;
                    output.thumbnailPixels = &actualThumbnail;
                    if (!motioncam::colorGalleryRgbVulkan(actual, gains, matrix, matrix,
                            1.0f, nullptr, nullptr, false, false, nullptr, &output, &texture) ||
                        actual != expected || actualThumbnail.empty() ||
                        actualThumbnail != expectedThumbnail) {
                        std::fprintf(stderr, "Resident texture clipping differs from CPU mask\n");
                        return 1;
                    }
                }
            }
            texture.black.fill(0);
            texture.white.fill(65535);
        }
        {
            motioncam::VulkanGalleryOutput output;
            output.width=output.height=16;output.rgb24=true;
            output.thumbnailWidth=5;output.thumbnailHeight=3;
            output.thumbnailGeneration=91;output.thumbnailSourceFrame=1;
            std::vector<uint8_t> thumbnail,clipping(256,4);
            int completed=-1;
            output.thumbnailPixels=&thumbnail;output.completedThumbnailSourceFrame=&completed;
            for(bool enabled:{true,false,true,false}){
                output.clipping=enabled?&clipping:nullptr;
                bool received=false;
                for(int attempt=0;attempt<32;++attempt){
                    std::vector<uint8_t> actual;
                    if(!motioncam::colorGalleryRgbVulkan(actual,gains,matrix,matrix,1.0f,
                        nullptr,nullptr,false,false,nullptr,&output,&texture))return 1;
                    if(thumbnail.empty())continue;
                    for(uint32_t y=0;y<3;++y)for(uint32_t x=0;x<5;++x){
                        const uint32_t sx=(2*x+1)*16/10,sy=(2*y+1)*16/6;
                        for(uint32_t c=0;c<3;++c)if(thumbnail[(y*5+x)*3+c]!=actual[(sy*16+sx)*3+c]){
                            std::fprintf(stderr,"Texture thumbnail retained stale clipping\n");return 1;
                        }
                    }
                    received=true;if(attempt>=3)break;
                }
                if(!received)return 1;
            }
        }
        motioncam::releaseVulkanGalleryTextureDevice(vk->gpu);
        pl_tex_destroy(vk->gpu, &tex);
        pl_vulkan_destroy(&vk);
    }
    constexpr uint32_t width = 32, height = 32;
    const std::vector<float> cfaPhaseGrid(4, 1.25f);
    const std::vector<float> rightHalfGrid(4, 1.5f);
    const std::vector<float> rgbGrid{
        1.25f, 1.0f, 0.75f, 1.25f, 1.0f, 0.75f,
        1.25f, 1.0f, 0.75f, 1.25f, 1.0f, 0.75f};
    auto gainMap = [](const std::vector<float>& data, uint32_t channels,
                      uint32_t left, uint32_t right, uint32_t pitch) {
        motioncam::VulkanRgbGainMap map;
        map.width = map.height = 2;
        map.channels = channels;
        map.left = left;
        map.top = 0;
        map.right = right;
        map.bottom = height;
        map.rowPitch = map.colPitch = pitch;
        map.spacingH = map.spacingV = 1.0f;
        map.data = &data;
        return map;
    };
    auto params = [&](uint32_t channels) {
        motioncam::VulkanGainBakeParams result;
        result.width = width;
        result.height = height;
        result.channels = channels;
        result.outputBits = 16;
        result.destinationWhite = 65535.0f;
        result.scale.fill(1.0f);
        return result;
    };
    auto run = [](const std::vector<uint16_t>& source,
                  const motioncam::VulkanGainBakeParams& settings,
                  bool cfa, std::vector<uint16_t>& result,
                  bool allowSkip = false) {
        result.resize(source.size());
        const bool ok = cfa
            ? motioncam::bakeCfaGainVulkan(source.data(), settings,
                reinterpret_cast<uint8_t*>(result.data()), result.size() * 2)
            : motioncam::bakeRgbGainVulkan(source.data(), settings,
                reinterpret_cast<uint8_t*>(result.data()), result.size() * 2);
        if (!ok) {
            std::fprintf(stderr, "Vulkan gain bake %s\n",
                allowSkip ? "unavailable; test skipped" : "failed");
            std::exit(allowSkip ? 77 : 1);
        }
        return ok;
    };

    std::vector<uint16_t> cfaSource(width * height, 1000), output;
    auto cfa = params(1);
    cfa.maps = {gainMap(cfaPhaseGrid, 1, 0, width, 2),
                gainMap(rightHalfGrid, 1, 16, width, 1)};
    run(cfaSource, cfa, true, output, true);
    if (output[0] != 1250 || output[1] != 1000 ||
        output[16] != 1875 || output[17] != 1500 ||
        output[width] != 1000) {
        std::fprintf(stderr, "CFA gain layout mismatch\n");
        return 1;
    }

    std::vector<uint16_t> rgbSource(width * height * 3, 1000);
    auto rgb = params(3);
    rgb.maps = {gainMap(rgbGrid, 3, 0, width, 1),
                gainMap(rightHalfGrid, 1, 16, width, 1)};
    run(rgbSource, rgb, false, output);
    if ((std::array<uint16_t, 3>{output[0], output[1], output[2]} !=
         std::array<uint16_t, 3>{1250, 1000, 750})) {
        std::fprintf(stderr, "RGB gain layout mismatch\n");
        return 1;
    }
    const size_t right = 16u * 3u;
    if ((std::array<uint16_t, 3>{output[right], output[right + 1],
                                 output[right + 2]} !=
         std::array<uint16_t, 3>{1875, 1500, 1125})) {
        std::fprintf(stderr, "RGB second-map layout mismatch\n");
        return 1;
    }
    const std::vector<float> interpolatedGrid{1.0f, 2.0f, 3.0f, 4.0f};
    auto interpolated = params(1);
    auto interpolatedMap = gainMap(interpolatedGrid, 1, 0, width, 1);
    interpolatedMap.spacingH = interpolatedMap.spacingV = 0.5f;
    interpolated.maps = {interpolatedMap};
    run(cfaSource, interpolated, true, output);
    if (output[7u * width + 7u] != 2406 ||
        output[15u * width + 15u] != 3906) {
        std::fprintf(stderr, "CFA gain interpolation mismatch\n");
        return 1;
    }
    const std::vector<float> colorGrid{
        1.1f, 1.3f, 1.7f, 1.1f, 1.3f, 1.7f,
        1.1f, 1.3f, 1.7f, 1.1f, 1.3f, 1.7f};
    auto phased = params(1);
    phased.cfaPhase = {2, 1, 1, 0};
    phased.maps = {gainMap(colorGrid, 3, 0, width, 1)};
    run(cfaSource, phased, true, output);
    if (output[0] != 1700 || output[1] != 1300 ||
        output[width] != 1300 || output[width + 1] != 1100) {
        std::fprintf(stderr, "CFA color phase mismatch\n");
        return 1;
    }
    return 0;
}

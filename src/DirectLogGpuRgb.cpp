#ifdef __ANDROID__
#include "android/VulkanHardwareBuffer.h"
#include "android/VulkanInteropFence.h"
#include <media/NdkImage.h>
#include <dlfcn.h>
#endif
#include "DirectLogGpuRgb.h"
#include "SharedVulkanDevice.h"
#include "VulkanGainBake.h"
#include "VulkanJpegFullEncode.h"
#include "VulkanGalleryPreview.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}
#include <libplacebo/filters.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <QDebug>
#endif

namespace {

void lockQueue(void* context, uint32_t family, uint32_t index) {
    auto* device = static_cast<AVHWDeviceContext*>(context);
    auto* vulkan = static_cast<AVVulkanDeviceContext*>(device->hwctx);
    vulkan->lock_queue(device, family, index);
}

void unlockQueue(void* context, uint32_t family, uint32_t index) {
    auto* device = static_cast<AVHWDeviceContext*>(context);
    auto* vulkan = static_cast<AVVulkanDeviceContext*>(device->hwctx);
    vulkan->unlock_queue(device, family, index);
}

} // namespace
#endif

namespace motioncam {

const char* DirectLogGpuRgb::transferShader() {
    // Quantization matches the RGBA16 intermediate used by the ordinary path.
    return R"glsl(
vec3 motioncamDecodeDirectLog(vec3 encoded, uint curve) {
    if (curve == 0u) return encoded;
    vec3 e = floor(clamp(encoded, 0.0, 1.0) * 65535.0 + 0.5) / 65535.0;
    if (curve == 1u)
        return mix(e * e / 3.0,
            (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0,
            greaterThan(e, vec3(0.5)));
    return (pow(vec3(61.0), e) - 1.0) / 60.0;
}
)glsl";
}

struct DirectLogGpuRgb::State {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#ifdef __ANDROID__
    struct AndroidImport {
        AHardwareBuffer* buffer = nullptr;
        uint64_t id = 0;
        bool hasId = false;
        bool fullRange = false, smoothChroma = false;
        std::unique_ptr<VulkanHardwareBuffer> hardware;
    };
    struct AndroidInput {
        pl_vulkan vk = nullptr;
        pl_tex texture = nullptr;
        VkImage output = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore available = VK_NULL_HANDLE;
        std::shared_ptr<AndroidImport> imported;
        ~AndroidInput() {
            imported.reset();
            if (view) vkDestroyImageView(vk->device, view, nullptr);
            if (available) vkDestroySemaphore(vk->device, available, nullptr);
            if (texture) pl_tex_destroy(vk->gpu, &texture);
        }
    };
    std::vector<std::unique_ptr<AndroidInput>> androidInputs;
    std::vector<std::shared_ptr<AndroidImport>> androidImports;
#endif
    AVBufferRef* deviceRef = nullptr;
    AVFrame* output = nullptr;
    pl_log log = nullptr;
    pl_vulkan vulkan = nullptr;
    pl_renderer renderer = nullptr;
    pl_tex sourceTextures[4] = {};
    pl_tex textures[4] = {};
    pl_tex sampleableOutput = nullptr;
    pl_frame outputTarget = {};
    bool outputTargetReady = false;
    pl_frame pendingTarget = {};
    bool pending = false;
    const pl_hook* hlgHook = nullptr;
    const pl_hook* log60Hook = nullptr;
    const pl_hook* limited10Hook = nullptr;
    const pl_hook* mosaicHook = nullptr;
    std::array<uint8_t, 4> mosaicPhase{};
    bool bayer = false;
    bool rgbaOutput = false;
    std::vector<uint16_t> rgbaDownload;

    bool downloadRgb(const pl_frame& target, uint16_t* pixels) {
        if (!rgbaOutput)
            return pl_download_avframe(vulkan->gpu, &target, output);
        rgbaDownload.resize(static_cast<size_t>(width) * height * 4u);
        output->data[0] = reinterpret_cast<uint8_t*>(rgbaDownload.data());
        output->linesize[0] = width * 4 * sizeof(uint16_t);
        const bool ok = pl_download_avframe(vulkan->gpu, &target, output);
        if (ok) {
            const size_t count = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < count; ++i)
                std::memcpy(pixels + i * 3u, rgbaDownload.data() + i * 4u,
                            3u * sizeof(uint16_t));
        }
        return ok;
    }
    int width = 0;
    int height = 0;

    bool prepareForSeek() {
        if (!vulkan) return true;
        // Finish work that may still reference the decoder's input surface
        // before FFmpeg reuses it. The renderer and its output textures are
        // independent of the input AVFrame, so keep them warm across seeks.
        pl_gpu_finish(vulkan->gpu);
        pending = false;
        pendingTarget = {};
        return renderer != nullptr;
    }

    ~State() {
        if (vulkan) {
            releaseVulkanGainBakeTextureDevice(vulkan->gpu);
            releaseVulkanJpegFullEncodeDevice(vulkan->gpu);
            releaseVulkanGalleryTextureDevice(vulkan->gpu);
        }
        if (vulkan) pl_gpu_finish(vulkan->gpu);
#ifdef __ANDROID__
        androidInputs.clear();
        androidImports.clear();
#endif
        av_frame_free(&output);
        if (vulkan) {
            for (auto& texture : sourceTextures)
                pl_tex_destroy(vulkan->gpu, &texture);
            for (auto& texture : textures) pl_tex_destroy(vulkan->gpu, &texture);
            pl_tex_destroy(vulkan->gpu, &sampleableOutput);
        }
        pl_mpv_user_shader_destroy(&hlgHook);
        pl_mpv_user_shader_destroy(&log60Hook);
        pl_mpv_user_shader_destroy(&limited10Hook);
        pl_mpv_user_shader_destroy(&mosaicHook);
        pl_renderer_destroy(&renderer);
        pl_vulkan_destroy(&vulkan);
        pl_log_destroy(&log);
        av_buffer_unref(&deviceRef);
    }

    bool init(const AVFrame* source, int outputWidth, int outputHeight,
              bool outputBayer) {
        const bool softwareSource = !source->hw_frames_ctx;
        AVHWFramesContext* frames = softwareSource ? nullptr :
            reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
        if (!softwareSource && (!frames || !frames->device_ref)) return false;
        auto* device = softwareSource ? nullptr :
            reinterpret_cast<AVHWDeviceContext*>(frames->device_ref->data);
        if (!softwareSource && (!device || device->type != AV_HWDEVICE_TYPE_VULKAN))
            return false;
        auto* hw = softwareSource ? nullptr :
            static_cast<AVVulkanDeviceContext*>(device->hwctx);
        if (!softwareSource && (!hw || !hw->lock_queue || !hw->unlock_queue))
            return false;

        log = pl_log_create(PL_API_VER, nullptr);
        if (!log) return false;
        if (softwareSource) {
            vulkan = createGalleryVulkanContext(log);
        } else {
        pl_vulkan_import_params params = {};
        params.instance = hw->inst;
        params.get_proc_addr = hw->get_proc_addr;
        params.phys_device = hw->phys_dev;
        params.device = hw->act_dev;
        params.extensions = hw->enabled_dev_extensions;
        params.num_extensions = hw->nb_enabled_dev_extensions;
        params.features = &hw->device_features;
        params.lock_queue = lockQueue;
        params.unlock_queue = unlockQueue;
        params.queue_ctx = device;
        params.max_api_version = VK_API_VERSION_1_3;
        params.queue_graphics.index = VK_QUEUE_FAMILY_IGNORED;
        params.queue_compute.index = VK_QUEUE_FAMILY_IGNORED;
        params.queue_transfer.index = VK_QUEUE_FAMILY_IGNORED;
#if LIBAVUTIL_VERSION_MAJOR < 59
        // FFmpeg 6 exposes each Vulkan queue role as a separate field.
        // Later releases expose the queue families through qf[].
        if (hw->queue_family_index >= 0)
            params.queue_graphics = {uint32_t(hw->queue_family_index),
                                     uint32_t(hw->nb_graphics_queues)};
        if (hw->queue_family_comp_index >= 0)
            params.queue_compute = {uint32_t(hw->queue_family_comp_index),
                                    uint32_t(hw->nb_comp_queues)};
        if (hw->queue_family_tx_index >= 0)
            params.queue_transfer = {uint32_t(hw->queue_family_tx_index),
                                     uint32_t(hw->nb_tx_queues)};
#else
        for (int i = 0; i < hw->nb_qf; ++i) {
            const auto& queue = hw->qf[i];
            pl_vulkan_queue mapped = {};
            mapped.index = queue.idx;
            mapped.count = queue.num;
#if PL_API_VER >= 365
            mapped.flags = hw->queue_flags;
#endif
            if (queue.flags & VK_QUEUE_GRAPHICS_BIT) params.queue_graphics = mapped;
            if (queue.flags & VK_QUEUE_COMPUTE_BIT) params.queue_compute = mapped;
            if (queue.flags & VK_QUEUE_TRANSFER_BIT) params.queue_transfer = mapped;
        }
#endif
        vulkan = pl_vulkan_import(log, &params);
        }
        if (!vulkan) return false;
        renderer = pl_renderer_create(log, vulkan->gpu);
        if (!renderer) return false;
        static const std::string hlgShader =
            std::string("//!HOOK SCALED\n//!BIND HOOKED\n") + DirectLogGpuRgb::transferShader() +
            "vec4 hook() { vec4 c = HOOKED_texOff(0); c.rgb = motioncamDecodeDirectLog(c.rgb, 1u); return c; }\n";
        static const std::string log60Shader =
            std::string("//!HOOK SCALED\n//!BIND HOOKED\n") + DirectLogGpuRgb::transferShader() +
            "vec4 hook() { vec4 c = HOOKED_texOff(0); c.rgb = motioncamDecodeDirectLog(c.rgb, 2u); return c; }\n";
        static constexpr char limited10Shader[] = R"(
//!HOOK NATIVE
//!BIND HOOKED
vec4 hook() {
    vec4 c = HOOKED_texOff(0);
    c.rgb *= 1020.0 / 1023.0;
    return c;
}
)";
        hlgHook = pl_mpv_user_shader_parse(vulkan->gpu, hlgShader.data(), hlgShader.size());
        log60Hook = pl_mpv_user_shader_parse(vulkan->gpu, log60Shader.data(), log60Shader.size());
        limited10Hook = pl_mpv_user_shader_parse(vulkan->gpu, limited10Shader,
                                                sizeof(limited10Shader) - 1);
        if (!hlgHook || !log60Hook || !limited10Hook) return false;
        // Android Vulkan drivers can mishandle three-component 16-bit render
        // targets when the texture is sampled by the gallery. Use a native
        // four-component target, then discard alpha on CPU download.
#ifdef __ANDROID__
        rgbaOutput = !outputBayer;
#endif
        const auto outputFormat = outputBayer ? AV_PIX_FMT_GRAY16LE :
            rgbaOutput ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE;
        if (!pl_test_pixfmt(vulkan->gpu, outputFormat)) {
#ifdef __ANDROID__
            qWarning() << "DirectLog Vulkan output format unsupported"
                       << int(outputFormat);
#endif
            return false;
        }
        if (frames) deviceRef = av_buffer_ref(frames->device_ref);
        output = av_frame_alloc();
        if ((!softwareSource && !deviceRef) || !output) return false;
        output->format = outputFormat;
        output->width = outputWidth;
        output->height = outputHeight;
        output->color_range = AVCOL_RANGE_JPEG;
        output->colorspace = AVCOL_SPC_RGB;
        output->color_primaries = AVCOL_PRI_BT2020;
        output->color_trc = AVCOL_TRC_LINEAR;
        width = outputWidth;
        height = outputHeight;
        bayer = outputBayer;
        return true;
    }

    bool render(const AVFrame* source, bool smoothChroma,
                DirectLogGpuRgb::TransferCurve curve, bool fullRange,
                uint16_t* packedRgb,
                const std::array<uint8_t, 4>* cfaPhase,
                bool deferDownload, const pl_frame* externalInput = nullptr) {
        pending = false;
        output->data[0] = reinterpret_cast<uint8_t*>(packedRgb);
        output->linesize[0] = width * (cfaPhase ? 1 : rgbaOutput ? 4 : 3) * sizeof(uint16_t);
        AVFrame* tagged = av_frame_clone(source);
        if (!tagged) {
            output->data[0] = nullptr;
            output->linesize[0] = 0;
            return false;
        }
        tagged->color_trc = AVCOL_TRC_LINEAR;
        tagged->colorspace = AVCOL_SPC_BT2020_NCL;
        tagged->color_primaries = AVCOL_PRI_BT2020;
        tagged->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

        pl_avframe_params inputParams = {};
        inputParams.frame = tagged;
        if (!tagged->hw_frames_ctx) inputParams.tex = sourceTextures;
        inputParams.map_dovi = false;
        pl_frame input = externalInput ? *externalInput : pl_frame{};
        const auto mapStarted = std::chrono::steady_clock::now();
        const bool mapped = externalInput || pl_map_avframe_ex(vulkan->gpu, &input, &inputParams);
        av_frame_free(&tagged);
        if (!mapped) {
            output->data[0] = nullptr;
            output->linesize[0] = 0;
            return false;
        }
        // The decoder returns storage-oriented pixels. The gallery applies the
        // clip display transform later; rotating here stretches a 4:3 frame.
        input.rotation = PL_ROTATION_0;

        if (!outputTargetReady) {
            pl_frame target = {};
            if (pl_frame_recreate_from_avframe(vulkan->gpu, &target,
                                              textures, output) &&
                target.num_planes == 1 && target.planes[0].texture) {
                const auto* base = target.planes[0].texture;
                pl_tex_params sampleable = {};
                sampleable.w = base->params.w;
                sampleable.h = base->params.h;
                sampleable.format = base->params.format;
                sampleable.renderable = true;
                sampleable.sampleable = true;
                sampleable.host_readable = true;
                if (pl_tex_recreate(vulkan->gpu, &sampleableOutput, &sampleable)) {
                    target.planes[0].texture = sampleableOutput;
                    outputTarget = target;
                    outputTargetReady = true;
                    pl_tex_destroy(vulkan->gpu, &textures[0]);
                }
            }
        }
        pl_frame target = outputTarget;
        bool rendered = false;
        if (outputTargetReady) {
            pl_render_params params = pl_render_default_params;
            params.upscaler = smoothChroma ? &pl_filter_mitchell : &pl_filter_nearest;
            // Proxy rendering averages the source area represented by each
            // output pixel. This also avoids ringing at high-contrast edges.
            params.downscaler = source->width > width || source->height > height
                ? &pl_filter_box : &pl_filter_mitchell;
            params.plane_upscaler = params.upscaler;
            params.peak_detect_params = nullptr;
            params.deband_params = nullptr;
            const pl_hook* transferHook = curve == DirectLogGpuRgb::TransferCurve::HLG
                ? hlgHook : curve == DirectLogGpuRgb::TransferCurve::LOG60
                    ? log60Hook : nullptr;
            const auto* frames = source->hw_frames_ctx
                ? reinterpret_cast<const AVHWFramesContext*>(
                    source->hw_frames_ctx->data) : nullptr;
            const pl_hook* hooks[3] = {};
            if (!fullRange && frames && frames->sw_format == AV_PIX_FMT_P010LE)
                hooks[params.num_hooks++] = limited10Hook;
            if (transferHook) hooks[params.num_hooks++] = transferHook;
            if (cfaPhase) {
                if (!mosaicHook || mosaicPhase != *cfaPhase) {
                    pl_mpv_user_shader_destroy(&mosaicHook);
                    mosaicPhase = *cfaPhase;
                    const std::string shader =
                        "//!HOOK SCALED\n//!BIND HOOKED\n"
                        "vec4 hook() {\n"
                        "    vec3 c = HOOKED_texOff(0).rgb;\n"
                        "    ivec2 p = ivec2(floor(HOOKED_pos * HOOKED_size));\n"
                        "    ivec4 phase = ivec4(" +
                        std::to_string((*cfaPhase)[0]) + "," +
                        std::to_string((*cfaPhase)[1]) + "," +
                        std::to_string((*cfaPhase)[2]) + "," +
                        std::to_string((*cfaPhase)[3]) + ");\n"
                        "    float v = c[phase[(p.y & 1) * 2 + (p.x & 1)]];\n"
                        "    return vec4(v, v, v, 1.0);\n"
                        "}\n";
                    mosaicHook = pl_mpv_user_shader_parse(vulkan->gpu,
                        shader.c_str(), shader.size());
                }
                if (!mosaicHook) {
                    if (!externalInput) pl_unmap_avframe(vulkan->gpu, &input);
                    pl_gpu_flush(vulkan->gpu);
                    output->data[0] = nullptr;
                    output->linesize[0] = 0;
                    return false;
                }
                hooks[params.num_hooks++] = mosaicHook;
            }
            params.hooks = hooks;
            const auto renderStart = std::chrono::steady_clock::now();
            const bool renderOk = pl_render_image(renderer, &input, &target, &params);
            const auto downloadStart = std::chrono::steady_clock::now();
            rendered = renderOk && (deferDownload || downloadRgb(target, packedRgb));
            if (rendered && deferDownload) {
                pendingTarget = target;
                pending = true;
            }
            const auto downloadEnd = std::chrono::steady_clock::now();
            if (std::getenv("MOTIONCAM_DIRECTLOG_GPU_RGB_PROFILE")) {
                const auto ms = [](auto a, auto b) {
                    return std::chrono::duration<double, std::milli>(b - a).count();
                };
                std::fprintf(stderr, "direct GPU RGB render_ms=%.3f download_ms=%.3f\n",
                             ms(renderStart, downloadStart), ms(downloadStart, downloadEnd));
            }
        }
        if (!externalInput) pl_unmap_avframe(vulkan->gpu, &input);
        pl_gpu_flush(vulkan->gpu);
        output->data[0] = nullptr;
        output->linesize[0] = 0;
        return rendered;
    }
#endif
};

DirectLogGpuRgb::DirectLogGpuRgb() : mState(std::make_shared<State>()) {}
DirectLogGpuRgb::~DirectLogGpuRgb() = default;

bool DirectLogGpuRgb::render(const AVFrame* source, int width, int height,
                             bool smoothChroma, TransferCurve curve,
                             bool fullRange, uint16_t* packedRgb,
                             const std::array<uint8_t, 4>* cfaPhase,
                             bool deferDownload) {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    if (!source || !packedRgb) return false;
    auto* frames = source->hw_frames_ctx ?
        reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data) : nullptr;
    if (source->hw_frames_ctx && (!frames || !frames->device_ref)) return false;
    if (!mState->vulkan ||
        bool(mState->deviceRef) != bool(frames) ||
        (frames && mState->deviceRef->data != frames->device_ref->data) ||
        mState->width != width || mState->height != height ||
        mState->bayer != static_cast<bool>(cfaPhase)) {
        mState = std::make_shared<State>();
        if (!mState->init(source, width, height, cfaPhase != nullptr)) return false;
    }
    return mState->render(source, smoothChroma, curve, fullRange, packedRgb,
                          cfaPhase, deferDownload);
#else
    (void)source; (void)width; (void)height; (void)smoothChroma;
    (void)curve; (void)fullRange; (void)packedRgb; (void)cfaPhase;
    (void)deferDownload;
    return false;
#endif
}

#ifdef __ANDROID__
bool DirectLogGpuRgb::renderAndroidImage(const AVFrame* source, AImage* image,
        int acquireFenceFd, int width, int height, bool smoothChroma,
        TransferCurve curve, bool fullRange, uint16_t* packedRgb,
        const std::array<uint8_t, 4>* cfaPhase, bool deferDownload) {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    auto fail = [&] {
        if (acquireFenceFd >= 0) ::close(acquireFenceFd);
        if (image) AImage_delete(image);
        return false;
    };
    if (!source || !image || !packedRgb || source->hw_frames_ctx) return fail();
    if (!mState->vulkan || mState->deviceRef || mState->width != width ||
        mState->height != height || mState->bayer != bool(cfaPhase)) {
        mState = std::make_shared<State>();
        if (!mState->init(source, width, height, bool(cfaPhase))) return fail();
    }
    AHardwareBuffer* hardware = nullptr;
    if (AImage_getHardwareBuffer(image, &hardware) != AMEDIA_OK) return fail();
    using GetBufferId = int (*)(const AHardwareBuffer*, uint64_t*);
    static const auto getBufferId = reinterpret_cast<GetBufferId>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId"));
    uint64_t bufferId = 0;
    const bool hasBufferId = getBufferId && getBufferId(hardware, &bufferId) == 0;
    std::unique_ptr<State::AndroidInput> lease;
    for (auto it = mState->androidInputs.begin(); it != mState->androidInputs.end(); ++it) {
        if (!pl_tex_poll(mState->vulkan->gpu, (*it)->texture, 0)) {
            lease = std::move(*it);
            mState->androidInputs.erase(it);
            break;
        }
    }
    // Import caching must not retain a full-resolution RGBA image per codec
    // allocation. Reuse at most three output images independently of imports.
    if (!lease && mState->androidInputs.size() >= 3) {
        while (pl_tex_poll(mState->vulkan->gpu, mState->androidInputs.front()->texture,
                           UINT64_MAX)) {}
        lease = std::move(mState->androidInputs.front());
        mState->androidInputs.erase(mState->androidInputs.begin());
    }
    if (!lease) {
        lease = std::make_unique<State::AndroidInput>();
        lease->vk = mState->vulkan;
        pl_fmt format = pl_find_fmt(lease->vk->gpu, PL_FMT_UNORM, 4, 16, 16,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
        if (!format) return fail();
        pl_tex_params textureInfo{};
        textureInfo.w = source->width; textureInfo.h = source->height;
        textureInfo.format = format;
        textureInfo.sampleable = textureInfo.storable = true;
        lease->texture = pl_tex_create(lease->vk->gpu, &textureInfo);
        if (!lease->texture) return fail();
        VkFormat vkFormat = VK_FORMAT_UNDEFINED;
        lease->output = pl_vulkan_unwrap(lease->vk->gpu, lease->texture, &vkFormat, nullptr);
        if (vkFormat != VK_FORMAT_R16G16B16A16_UNORM) return fail();
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = lease->output; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = vkFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(lease->vk->device, &view, nullptr, &lease->view) != VK_SUCCESS)
            return fail();
        lease->available = createAndroidReleaseSemaphore(lease->vk);
        if (!lease->available) return fail();
    }
    lease->imported.reset();
    for (auto it = mState->androidImports.begin(); it != mState->androidImports.end();) {
        // Only reuse submission resources after their output read has completed.
        const bool sameBuffer = hasBufferId && (*it)->hasId
            ? (*it)->id == bufferId : (*it)->buffer == hardware;
        if (it->use_count() == 1 && sameBuffer &&
            (*it)->fullRange == fullRange && (*it)->smoothChroma == smoothChroma) {
            lease->imported = *it;
            break;
        }
        if (it->use_count() == 1 && mState->androidImports.size() >= 8)
            it = mState->androidImports.erase(it);
        else ++it;
    }
    if (!lease->imported) {
        auto imported = std::make_shared<State::AndroidImport>();
        imported->buffer = hardware;
        imported->id = bufferId;
        imported->hasId = hasBufferId;
        imported->fullRange = fullRange;
        imported->smoothChroma = smoothChroma;
        imported->hardware = std::make_unique<VulkanHardwareBuffer>();
        if (!imported->hardware->initialize(lease->vk, hardware, fullRange, smoothChroma) ||
            imported->hardware->width != static_cast<uint32_t>(source->width) ||
            imported->hardware->height != static_cast<uint32_t>(source->height)) return fail();
        lease->imported = imported;
        mState->androidImports.push_back(std::move(imported));
    }
    pl_vulkan_hold_params hold{};
    hold.tex = lease->texture;
    hold.layout = VK_IMAGE_LAYOUT_GENERAL;
    hold.qf = lease->vk->queue_compute.index;
    hold.semaphore = {lease->available, 0};
    if (!pl_vulkan_hold_ex(lease->vk->gpu, &hold)) return fail();
    const char* fusionSetting = std::getenv("MOTIONCAM_ANDROID_FUSED_DECODE");
    const bool fused = deferDownload && !cfaPhase &&
        width == source->width && height == source->height &&
        (!fusionSetting || fusionSetting[0] != '0');
    VkSemaphore ready = VK_NULL_HANDLE;
    int releaseFd = -1;
    const int consumedFd = acquireFenceFd;
    acquireFenceFd = -1;
    if (!lease->imported->hardware->submitSampling(lease->output, lease->view, VK_IMAGE_LAYOUT_GENERAL,
            consumedFd, lease->available, &ready, &releaseFd,
            fused ? curve : TransferCurve::None)) {
        pl_gpu_finish(lease->vk->gpu);
        return fail();
    }
    AImage_deleteAsync(image, releaseFd);
    image = nullptr;
    pl_vulkan_release_params release{};
    release.tex = lease->texture;
    release.layout = VK_IMAGE_LAYOUT_GENERAL;
    release.qf = lease->vk->queue_compute.index;
    release.semaphore = {ready, 0};
    pl_vulkan_release_ex(lease->vk->gpu, &release);
    pl_frame input{};
    input.num_planes = 1;
    input.planes[0].texture = lease->texture;
    input.planes[0].components = 4;
    for (int i = 0; i < 4; ++i) input.planes[0].component_mapping[i] = i;
    input.repr = pl_color_repr_rgb;
    input.repr.alpha = PL_ALPHA_NONE;
    input.color = pl_color_space_bt2020_hlg;
    input.color.transfer = PL_COLOR_TRC_LINEAR;
    bool rendered = true;
    if (fused) {
        // YCbCr sampling already produced the linear RGB texture. Pass that
        // allocation to the gallery directly instead of rendering it again.
        mState->pendingTarget = input;
        mState->pending = true;
    } else {
        rendered = mState->render(source, smoothChroma, curve, fullRange,
            packedRgb, cfaPhase, deferDownload, &input);
    }
    if (std::getenv("MOTIONCAM_GALLERY_PERF_PROFILE"))
        spdlog::info("GALLERY_PERF event=android_decode_pipeline fused={} width={} height={}",
            fused, width, height);
    // Keep both the imported decoder allocation and output sampler alive until
    // libplacebo's queued reads have completed; recycle without a host wait.
    mState->androidInputs.push_back(std::move(lease));
    return rendered;
#else
    (void)source; (void)width; (void)height; (void)smoothChroma; (void)curve;
    (void)fullRange; (void)packedRgb; (void)cfaPhase; (void)deferDownload;
    if (acquireFenceFd >= 0) ::close(acquireFenceFd);
    if (image) AImage_delete(image);
    return false;
#endif
}
#endif

std::shared_ptr<void> DirectLogGpuRgb::deviceLease() const { return mState; }

bool DirectLogGpuRgb::pendingTexture(const void*& gpu, const void*& texture) const {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    if (!mState->pending || !mState->vulkan || !mState->pendingTarget.planes[0].texture)
        return false;
    gpu = mState->vulkan->gpu;
    texture = mState->pendingTarget.planes[0].texture;
    return true;
#else
    (void)gpu; (void)texture;
    return false;
#endif
}

bool DirectLogGpuRgb::downloadPending(uint16_t* pixels, size_t samples) {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    if (std::getenv("MOTIONCAM_GALLERY_REQUIRE_ZERO_COPY")) {
        spdlog::error("Full-frame DirectLog readback rejected by zero-copy validation");
        return false;
    }
    if (!mState->pending || !pixels || samples != static_cast<size_t>(mState->width) *
        mState->height * (mState->bayer ? 1u : 3u)) return false;
    mState->output->data[0] = reinterpret_cast<uint8_t*>(pixels);
    mState->output->linesize[0] = mState->width *
        (mState->bayer ? 1 : mState->rgbaOutput ? 4 : 3) * sizeof(uint16_t);
    const bool ok = mState->downloadRgb(mState->pendingTarget, pixels);
    mState->output->data[0] = nullptr;
    mState->output->linesize[0] = 0;
    mState->pending = false;
    return ok;
#else
    (void)pixels; (void)samples;
    return false;
#endif
}

bool DirectLogGpuRgb::prepareForSeek() {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    return mState->prepareForSeek();
#else
    return true;
#endif
}

} // namespace motioncam

#include "DirectLogGpuRgb.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}
#include <libplacebo/filters.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

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

struct DirectLogGpuRgb::State {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    AVBufferRef* deviceRef = nullptr;
    AVFrame* output = nullptr;
    pl_log log = nullptr;
    pl_vulkan vulkan = nullptr;
    pl_renderer renderer = nullptr;
    pl_tex textures[4] = {};
    const pl_hook* hlgHook = nullptr;
    const pl_hook* log60Hook = nullptr;
    const pl_hook* limited10Hook = nullptr;
    const pl_hook* mosaicHook = nullptr;
    std::array<uint8_t, 4> mosaicPhase{};
    bool bayer = false;
    int width = 0;
    int height = 0;

    ~State() {
        if (vulkan) pl_gpu_finish(vulkan->gpu);
        av_frame_free(&output);
        if (vulkan) {
            for (auto& texture : textures) pl_tex_destroy(vulkan->gpu, &texture);
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
        auto* frames = reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
        if (!frames || !frames->device_ref) return false;
        auto* device = reinterpret_cast<AVHWDeviceContext*>(frames->device_ref->data);
        if (!device || device->type != AV_HWDEVICE_TYPE_VULKAN) return false;
        auto* hw = static_cast<AVVulkanDeviceContext*>(device->hwctx);
        if (!hw || !hw->lock_queue || !hw->unlock_queue) return false;

        log = pl_log_create(PL_API_VER, nullptr);
        if (!log) return false;
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
        vulkan = pl_vulkan_import(log, &params);
        if (!vulkan) return false;
        renderer = pl_renderer_create(log, vulkan->gpu);
        if (!renderer) return false;
        static constexpr char hlgShader[] = R"(
//!HOOK SCALED
//!BIND HOOKED
vec4 hook() {
    vec4 c = HOOKED_texOff(0);
    vec3 e = floor(clamp(c.rgb, 0.0, 1.0) * 65535.0 + 0.5) / 65535.0;
    c.rgb = mix(e * e / 3.0,
                (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0,
                greaterThan(e, vec3(0.5)));
    return c;
}
)";
        static constexpr char log60Shader[] = R"(
//!HOOK SCALED
//!BIND HOOKED
vec4 hook() {
    vec4 c = HOOKED_texOff(0);
    vec3 e = floor(clamp(c.rgb, 0.0, 1.0) * 65535.0 + 0.5) / 65535.0;
    c.rgb = (pow(vec3(61.0), e) - 1.0) / 60.0;
    return c;
}
)";
        static constexpr char limited10Shader[] = R"(
//!HOOK NATIVE
//!BIND HOOKED
vec4 hook() {
    vec4 c = HOOKED_texOff(0);
    c.rgb *= 1020.0 / 1023.0;
    return c;
}
)";
        hlgHook = pl_mpv_user_shader_parse(vulkan->gpu, hlgShader, sizeof(hlgShader) - 1);
        log60Hook = pl_mpv_user_shader_parse(vulkan->gpu, log60Shader, sizeof(log60Shader) - 1);
        limited10Hook = pl_mpv_user_shader_parse(vulkan->gpu, limited10Shader,
                                                sizeof(limited10Shader) - 1);
        if (!hlgHook || !log60Hook || !limited10Hook) return false;
        const auto outputFormat = outputBayer ? AV_PIX_FMT_GRAY16LE : AV_PIX_FMT_RGB48LE;
        if (!pl_test_pixfmt(vulkan->gpu, outputFormat)) return false;
        deviceRef = av_buffer_ref(frames->device_ref);
        output = av_frame_alloc();
        if (!deviceRef || !output) return false;
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
                const std::array<uint8_t, 4>* cfaPhase) {
        output->data[0] = reinterpret_cast<uint8_t*>(packedRgb);
        output->linesize[0] = width * (cfaPhase ? 1 : 3) * sizeof(uint16_t);
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
        inputParams.map_dovi = false;
        pl_frame input = {};
        const bool mapped = pl_map_avframe_ex(vulkan->gpu, &input, &inputParams);
        av_frame_free(&tagged);
        if (!mapped) {
            output->data[0] = nullptr;
            output->linesize[0] = 0;
            return false;
        }
        // The decoder returns storage-oriented pixels. The gallery applies the
        // clip display transform later; rotating here stretches a 4:3 frame.
        input.rotation = PL_ROTATION_0;

        pl_frame target = {};
        const bool targetReady = pl_frame_recreate_from_avframe(
            vulkan->gpu, &target, textures, output);
        bool rendered = false;
        if (targetReady) {
            pl_render_params params = pl_render_default_params;
            params.upscaler = smoothChroma ? &pl_filter_mitchell : &pl_filter_nearest;
            params.downscaler = &pl_filter_mitchell;
            params.plane_upscaler = params.upscaler;
            params.peak_detect_params = nullptr;
            params.deband_params = nullptr;
            const pl_hook* transferHook = curve == DirectLogGpuRgb::TransferCurve::HLG
                ? hlgHook : curve == DirectLogGpuRgb::TransferCurve::LOG60
                    ? log60Hook : nullptr;
            const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
                source->hw_frames_ctx->data);
            const pl_hook* hooks[3] = {};
            if (!fullRange && frames->sw_format == AV_PIX_FMT_P010LE)
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
                    pl_unmap_avframe(vulkan->gpu, &input);
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
            rendered = renderOk && pl_download_avframe(vulkan->gpu, &target, output);
            const auto downloadEnd = std::chrono::steady_clock::now();
            if (std::getenv("MOTIONCAM_DIRECTLOG_GPU_RGB_PROFILE")) {
                const auto ms = [](auto a, auto b) {
                    return std::chrono::duration<double, std::milli>(b - a).count();
                };
                std::fprintf(stderr, "direct GPU RGB render_ms=%.3f download_ms=%.3f\n",
                             ms(renderStart, downloadStart), ms(downloadStart, downloadEnd));
            }
        }
        pl_unmap_avframe(vulkan->gpu, &input);
        pl_gpu_flush(vulkan->gpu);
        output->data[0] = nullptr;
        output->linesize[0] = 0;
        return rendered;
    }
#endif
};

DirectLogGpuRgb::DirectLogGpuRgb() : mState(std::make_unique<State>()) {}
DirectLogGpuRgb::~DirectLogGpuRgb() = default;

bool DirectLogGpuRgb::render(const AVFrame* source, int width, int height,
                             bool smoothChroma, TransferCurve curve,
                             bool fullRange, uint16_t* packedRgb,
                             const std::array<uint8_t, 4>* cfaPhase) {
#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
    if (!source || !source->hw_frames_ctx || !packedRgb) return false;
    auto* frames = reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
    if (!frames || !frames->device_ref) return false;
    if (!mState->deviceRef ||
        mState->deviceRef->data != frames->device_ref->data ||
        mState->width != width || mState->height != height ||
        mState->bayer != static_cast<bool>(cfaPhase)) {
        mState = std::make_unique<State>();
        if (!mState->init(source, width, height, cfaPhase != nullptr)) return false;
    }
    return mState->render(source, smoothChroma, curve, fullRange, packedRgb,
                          cfaPhase);
#else
    (void)source; (void)width; (void)height; (void)smoothChroma;
    (void)curve; (void)fullRange; (void)packedRgb; (void)cfaPhase;
    return false;
#endif
}

} // namespace motioncam

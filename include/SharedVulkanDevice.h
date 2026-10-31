#pragma once

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/vulkan.h>
#ifdef __ANDROID__
#include "android/VulkanInteropDevice.h"
#include <android/log.h>
#endif

namespace motioncam {
// Keep the physical device, enabled features and queue locks common to all
// stages. The device outlives Qt and source-frame leases.
inline pl_vulkan sharedVulkanDevice() {
    static const pl_vulkan device = [] {
        pl_log log = pl_log_create(PL_API_VER, nullptr);
        if (!log) return pl_vulkan{};
        pl_vk_inst_params instanceParams = pl_vk_inst_default_params;
#ifdef __ANDROID__
        static constexpr const char* instanceExtensions[] = {
            "VK_KHR_surface", "VK_KHR_android_surface"
        };
        instanceParams.extensions = instanceExtensions;
        instanceParams.num_extensions = 2;
#endif
        pl_vulkan_params params = pl_vulkan_default_params;
        params.instance_params = &instanceParams;
        static constexpr const char* deviceExtensions[] = {"VK_KHR_swapchain"};
        params.extensions = deviceExtensions;
        params.num_extensions = 1;
#ifdef __ANDROID__
        enableAndroidVulkanInterop(params);
#endif
        pl_vulkan result = pl_vulkan_create(log, &params);
        if (!result) pl_log_destroy(&log);
        return result;
    }();
    return device;
}
// Each processing stage needs its own command recorder. In libplacebo 7.360,
// swapchain submission can steal a pending command buffer, so sharing one
// pl_gpu concurrently with a presenter is unsafe even on a thread-safe GPU.
inline pl_vulkan createSharedVulkanContext(pl_log log) {
#ifdef __ANDROID__
    pl_log_params logParams{};
    logParams.log_level = PL_LOG_WARN;
    logParams.log_cb = [](void*, pl_log_level, const char* message) {
        __android_log_write(ANDROID_LOG_WARN, "mcFuseVulkan", message);
    };
    if (log) pl_log_update(log, &logParams);
#endif
    const auto owner = sharedVulkanDevice();
    if (!owner) return nullptr;
    pl_vulkan_import_params params{};
    params.instance = owner->instance;
    params.get_proc_addr = owner->get_proc_addr;
    params.phys_device = owner->phys_device;
    params.device = owner->device;
    params.extensions = owner->extensions;
    params.num_extensions = owner->num_extensions;
    params.features = owner->features;
    params.queue_graphics = owner->queue_graphics;
    params.queue_compute = owner->queue_compute;
    params.queue_transfer = owner->queue_transfer;
    params.queue_ctx = const_cast<pl_vulkan_t*>(owner);
    params.lock_queue = [](void* ctx, uint32_t family, uint32_t index) {
        auto device = static_cast<pl_vulkan>(ctx);
        device->lock_queue(device, family, index);
    };
    params.unlock_queue = [](void* ctx, uint32_t family, uint32_t index) {
        auto device = static_cast<pl_vulkan>(ctx);
        device->unlock_queue(device, family, index);
    };
    return pl_vulkan_import(log, &params);
}
}
#endif

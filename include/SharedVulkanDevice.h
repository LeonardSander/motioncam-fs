#pragma once

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/vulkan.h>
#include "VulkanDevicePolicy.h"
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#ifdef __ANDROID__
#include "android/VulkanInteropDevice.h"
#include <android/log.h>
#endif

namespace motioncam {
inline PFN_vkGetInstanceProcAddr galleryVulkanGetInstanceProcAddr() {
#ifdef _WIN32
    // Keep the loader alive with the process's Vulkan devices. No static DLL
    // import: systems without Vulkan must still reach CPU/Qt fallback.
    static const auto getProc = [] {
        const auto library = LoadLibraryExW(L"vulkan-1.dll", nullptr,
                                            LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library, "vkGetInstanceProcAddr")) : nullptr;
    }();
    return getProc;
#else
    // Other builds retain libplacebo's linked entrypoint.
    return nullptr;
#endif
}

// Keep the physical device, enabled features and queue locks common to all
// stages. The device outlives Qt and source-frame leases.
inline pl_vulkan sharedVulkanDevice() {
    static const pl_vulkan device = [] {
        const char* requested = std::getenv("MOTIONCAM_GALLERY_SHARED_DEVICE");
        if (requested && requested[0] == '0')
            return pl_vulkan{};
        const bool forced = requested && requested[0] == '1';
        pl_log log = pl_log_create(PL_API_VER, nullptr);
        if (!log) return pl_vulkan{};
        pl_vk_inst_params instanceParams = pl_vk_inst_default_params;
        instanceParams.get_proc_addr = galleryVulkanGetInstanceProcAddr();
#ifdef _WIN32
        if (!instanceParams.get_proc_addr) { pl_log_destroy(&log); return pl_vulkan{}; }
#endif
        static constexpr const char* surfaceExtension = "VK_KHR_surface";
        instanceParams.extensions = &surfaceExtension;
        instanceParams.num_extensions = 1;
        // Window-system extensions belong to the platform adapter, not to
        // unified-memory or image-sharing policy. Enable those available.
        static constexpr const char* surfaceExtensions[] = {
#ifdef __ANDROID__
            "VK_KHR_android_surface"
#elif defined(__APPLE__)
            "VK_EXT_metal_surface", "VK_MVK_macos_surface"
#elif defined(_WIN32)
            "VK_KHR_win32_surface"
#else
            "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"
#endif
        };
        instanceParams.opt_extensions = surfaceExtensions;
        instanceParams.num_opt_extensions = sizeof(surfaceExtensions) / sizeof(surfaceExtensions[0]);
        pl_vk_inst instance = pl_vk_inst_create(log, &instanceParams);
        if (!instance) { pl_log_destroy(&log); return pl_vulkan{}; }
        pl_vulkan_device_params selection{};
        selection.instance = instance->instance;
        selection.get_proc_addr = instance->get_proc_addr;
        VkPhysicalDevice physical = pl_vulkan_choose_device(log, &selection);
        if (!physical || (!forced && !vulkanUsesUnifiedMemory(
                instance->instance, physical, instance->get_proc_addr))) {
            pl_vk_inst_destroy(&instance);
            pl_log_destroy(&log);
            return pl_vulkan{};
        }
        pl_vulkan_params params = pl_vulkan_default_params;
        params.instance = instance->instance;
        params.get_proc_addr = instance->get_proc_addr;
        params.device = physical;
        static constexpr const char* deviceExtensions[] = {"VK_KHR_swapchain"};
        params.extensions = deviceExtensions;
        params.num_extensions = 1;
#ifdef __ANDROID__
        enableAndroidVulkanInterop(params);
#endif
        pl_vulkan result = pl_vulkan_create(log, &params);
        if (!result) { pl_vk_inst_destroy(&instance); pl_log_destroy(&log); }
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
    params.max_api_version = owner->api_version;
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
inline bool vulkanNeedsFenceCompletion(pl_vulkan device) {
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        device->get_proc_addr(device->instance, "vkGetPhysicalDeviceProperties2"));
    if (!getProperties) return true;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &driver;
    getProperties(device->phys_device, &properties);
    return driver.driverID == VK_DRIVER_ID_MOLTENVK;
}

inline pl_vulkan createGalleryVulkanContext(pl_log log) {
    if (auto context = createSharedVulkanContext(log)) return context;
    pl_vulkan_params params = pl_vulkan_default_params;
    params.get_proc_addr = galleryVulkanGetInstanceProcAddr();
#ifdef _WIN32
    if (!params.get_proc_addr) return nullptr;
#endif
#ifdef __ANDROID__
    enableAndroidVulkanInterop(params);
#endif
    return pl_vulkan_create(log, &params);
}
}
#endif

#pragma once

#if defined(__ANDROID__) && defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO)
#include <libplacebo/vulkan.h>

namespace motioncam {
// Both ends of the gallery handoff must enable the same interop extensions.
// Optional negotiation preserves devices that only support ordinary uploads.
inline void enableAndroidVulkanInterop(pl_vulkan_params& params) {
    static constexpr const char* extensions[] = {
        "VK_KHR_external_semaphore_fd",
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_EXT_queue_family_foreign"
    };
    static VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        nullptr, VK_TRUE
    };
    static const VkPhysicalDeviceFeatures2 features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        &ycbcr, {}
    };
    params.opt_extensions = extensions;
    params.num_opt_extensions = sizeof(extensions) / sizeof(extensions[0]);
    params.features = &features;
}
}
#endif

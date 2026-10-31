#pragma once

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include <libplacebo/vulkan.h>

namespace motioncam {
// A host-visible VRAM BAR does not turn a discrete GPU into unified memory.
inline bool vulkanMemoryIsUnified(const VkPhysicalDeviceProperties& properties,
                                 const VkPhysicalDeviceMemoryProperties& memory) {
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) return false;
    bool hasDeviceHeap = false;
    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if (!(memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        bool usable = false, hostVisible = false;
        for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
            const auto& candidate = memory.memoryTypes[type];
            if (candidate.heapIndex != heap ||
                !(candidate.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
                (candidate.propertyFlags & (VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                            VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT))) continue;
            usable = true;
            hostVisible |= bool(candidate.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        }
        if (usable) {
            hasDeviceHeap = true;
            if (!hostVisible) return false;
        }
    }
    return hasDeviceHeap;
}

inline bool vulkanUsesUnifiedMemory(VkInstance instance, VkPhysicalDevice device,
                                   PFN_vkGetInstanceProcAddr getProcAddr) {
#if defined(__ANDROID__) || (defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)))
    return true;
#else
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        getProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    const auto getMemory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        getProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!getProperties || !getMemory) return false;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    getProperties(device, &properties);
    getMemory(device, &memory);
    return vulkanMemoryIsUnified(properties, memory);
#endif
}

// External handle availability is independent of the memory topology. Other
// platforms retain host transport until their native adapters are implemented.
inline pl_handle_type galleryExternalBufferHandle(pl_gpu gpu) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (gpu->export_caps.buf & PL_HANDLE_DMA_BUF) return PL_HANDLE_DMA_BUF;
#endif
    return static_cast<pl_handle_type>(0);
}
}
#endif

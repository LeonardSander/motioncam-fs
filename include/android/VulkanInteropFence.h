#pragma once
#if defined(__ANDROID__) && defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO)
#include <libplacebo/vulkan.h>
#include <unistd.h>
#include <cstring>

namespace motioncam {
inline bool androidSemaphoreFdEnabled(pl_vulkan vk) {
    if (!vk) return false;
    for (int i = 0; i < vk->num_extensions; ++i)
        if (std::strcmp(vk->extensions[i], "VK_KHR_external_semaphore_fd") == 0)
            return true;
    return false;
}
// Import consumes fd on success; failures close it here. The caller retains
// the semaphore until all GPU submissions waiting on it have completed.
inline VkSemaphore importAndroidAcquireFence(pl_vulkan vk, int fd) {
    if (fd < 0) return VK_NULL_HANDLE;
    if (!androidSemaphoreFdEnabled(vk)) { ::close(fd); return VK_NULL_HANDLE; }
    auto importFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vk->get_proc_addr(vk->instance, "vkImportSemaphoreFdKHR"));
    if (!importFd) { ::close(fd); return VK_NULL_HANDLE; }
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(vk->device, &create, nullptr, &semaphore) != VK_SUCCESS) {
        ::close(fd);
        return VK_NULL_HANDLE;
    }
    VkImportSemaphoreFdInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import.semaphore = semaphore;
    import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import.fd = fd;
    if (importFd(vk->device, &import) != VK_SUCCESS) {
        ::close(fd);
        vkDestroySemaphore(vk->device, semaphore, nullptr);
        return VK_NULL_HANDLE;
    }
    return semaphore;
}

// Export only after queuing a signal on this binary semaphore. A returned
// sync_file fd can cross Vulkan devices and also serves AImage_deleteAsync.
inline int exportAndroidReleaseFence(pl_vulkan vk, VkSemaphore semaphore, bool* exported = nullptr) {
    if (exported) *exported = false;
    if (!androidSemaphoreFdEnabled(vk) || !semaphore) return -1;
    auto getFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vk->get_proc_addr(vk->instance, "vkGetSemaphoreFdKHR"));
    if (!getFd) return -1;
    VkSemaphoreGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    const bool success = getFd(vk->device, &info, &fd) == VK_SUCCESS;
    if (exported) *exported = success;
    return success ? fd : -1;
}

inline VkSemaphore createAndroidReleaseSemaphore(pl_vulkan vk) {
    if (!androidSemaphoreFdEnabled(vk)) return VK_NULL_HANDLE;
    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &exportInfo;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    return vkCreateSemaphore(vk->device, &create, nullptr, &semaphore) == VK_SUCCESS
        ? semaphore : VK_NULL_HANDLE;
}
}
#endif

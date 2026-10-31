#pragma once
#if defined(__ANDROID__) && defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO)
#include <android/hardware_buffer.h>
#include "DirectLogGpuRgb.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <libplacebo/vulkan.h>
namespace motioncam {
// Owns the Vulkan view of a decoder allocation. The AImage and its acquire /
// release fences remain the caller's responsibility; do not destroy in flight.
class VulkanHardwareBuffer {
public:
    using TransferCurve = DirectLogGpuRgb::TransferCurve;
    ~VulkanHardwareBuffer();
    VulkanHardwareBuffer() = default;
    VulkanHardwareBuffer(const VulkanHardwareBuffer&) = delete;
    VulkanHardwareBuffer& operator=(const VulkanHardwareBuffer&) = delete;
    bool initialize(pl_vulkan device, AHardwareBuffer* buffer,
                    bool fullRange, bool smoothChroma);
    // The output is a same-size RGBA16 UNORM storage image. Caller must wait the
    // Android acquire fence before executing this command buffer, signal a
    // release fence afterwards, and complete prior use before recording again.
    bool recordSampling(VkCommandBuffer command, VkImage output,
        VkImageView outputView, uint32_t queueFamily, VkImageLayout outputLayout,
        TransferCurve curve);

    // Reuse only after the previous libplacebo texture read completes.
    // Consumes acquireFenceFd on all
    // paths. On success, outputReady is waited by libplacebo, and releaseFd
    // goes to AImage_deleteAsync (including valid already-signalled -1).
    // Retain this object until the libplacebo wait has completed.
    bool submitSampling(VkImage output, VkImageView outputView,
        VkImageLayout outputLayout, int acquireFenceFd,
        VkSemaphore outputAvailable, VkSemaphore* outputReady, int* releaseFd,
        TransferCurve curve = TransferCurve::None);
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    uint64_t externalFormat = 0;
private:
    bool createSamplingPipeline();
    pl_vulkan mDevice = nullptr;
    AHardwareBuffer* mBuffer = nullptr;
    VkDeviceMemory mMemory = VK_NULL_HANDLE;
    VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer mCommand = VK_NULL_HANDLE;
    VkFence mCompletion = VK_NULL_HANDLE;
    VkSemaphore mAcquire = VK_NULL_HANDLE;
    VkSemaphore mOutputReady = VK_NULL_HANDLE;
    VkSemaphore mInputReleased = VK_NULL_HANDLE;
    bool mSubmitted = false;
};
}
#endif

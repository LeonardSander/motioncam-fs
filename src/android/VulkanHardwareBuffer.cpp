#include "android/VulkanHardwareBuffer.h"
#if defined(__ANDROID__) && defined(MOTIONCAM_HAS_DIRECTLOG_PLACEBO)
#include <spdlog/spdlog.h>
#include <shaderc/shaderc.h>
#include "android/VulkanInteropFence.h"
#include <limits>
#include <vector>
#include <string>
#include <cstring>
namespace motioncam {
VulkanHardwareBuffer::~VulkanHardwareBuffer() {
    if (!mDevice) return;
    const VkDevice device = mDevice->device;
    if (mSubmitted) vkWaitForFences(device, 1, &mCompletion, VK_TRUE,
        std::numeric_limits<uint64_t>::max());
    if (mCompletion) vkDestroyFence(device, mCompletion, nullptr);
    if (mCommandPool) vkDestroyCommandPool(device, mCommandPool, nullptr);
    if (mAcquire) vkDestroySemaphore(device, mAcquire, nullptr);
    if (mOutputReady) vkDestroySemaphore(device, mOutputReady, nullptr);
    if (mInputReleased) vkDestroySemaphore(device, mInputReleased, nullptr);
    if (mDescriptorPool) vkDestroyDescriptorPool(device, mDescriptorPool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (view) vkDestroyImageView(device, view, nullptr);
    if (conversion) vkDestroySamplerYcbcrConversion(device, conversion, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (mMemory) vkFreeMemory(device, mMemory, nullptr);
    if (mBuffer) AHardwareBuffer_release(mBuffer);
}
bool VulkanHardwareBuffer::createSamplingPipeline() {
    // External-format conversion requires an immutable combined sampler.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = &sampler;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(mDevice->device, &setInfo, nullptr,
            &descriptorLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    VkPushConstantRange curveRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &curveRange;
    if (vkCreatePipelineLayout(mDevice->device, &layoutInfo, nullptr,
            &pipelineLayout) != VK_SUCCESS) return false;
    static const std::string source = std::string(R"glsl(#version 450
layout(local_size_x=16, local_size_y=16) in;
layout(set=0, binding=0) uniform sampler2D decoderImage;
layout(set=0, binding=1, rgba16) writeonly uniform image2D rgbImage;
layout(push_constant) uniform Transfer { uint curve; } transfer;
)glsl") + DirectLogGpuRgb::transferShader() + R"glsl(
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dimensions = imageSize(rgbImage);
    if (any(greaterThanEqual(p, dimensions))) return;
    vec2 uv = (vec2(p) + 0.5) / vec2(dimensions);
    imageStore(rgbImage, p, vec4(motioncamDecodeDirectLog(
        texture(decoderImage, uv).rgb, transfer.curve), 1.0));
}
)glsl";
    // Source is device-independent. Compile once, including when a codec
    // cycles through more allocations than the bounded import cache retains.
    static const std::vector<uint32_t> spirv = [] {
        shaderc_compiler_t compiler = shaderc_compiler_initialize();
        if (!compiler) return std::vector<uint32_t>{};
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        if (!options) { shaderc_compiler_release(compiler); return std::vector<uint32_t>{}; }
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
            shaderc_env_version_vulkan_1_1);
        shaderc_compile_options_set_optimization_level(options,
            shaderc_optimization_level_performance);
        shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler,
            source.data(), source.size(), shaderc_compute_shader,
            "android-decoder-ycbcr.comp", "main", options);
        shaderc_compile_options_release(options);
        shaderc_compiler_release(compiler);
        if (!result) return std::vector<uint32_t>{};
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            spdlog::warn("MediaCodec YCbCr shader compilation failed: {}",
                shaderc_result_get_error_message(result));
            shaderc_result_release(result);
            return std::vector<uint32_t>{};
        }
        const size_t bytes = shaderc_result_get_length(result);
        std::vector<uint32_t> words(bytes / sizeof(uint32_t));
        std::memcpy(words.data(), shaderc_result_get_bytes(result), bytes);
        shaderc_result_release(result);
        return words;
    }();
    if (spirv.empty()) return false;
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult moduleResult = vkCreateShaderModule(mDevice->device,
        &moduleInfo, nullptr, &module);
    if (moduleResult != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(mDevice->device,
        VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(mDevice->device, module, nullptr);
    return pipelineResult == VK_SUCCESS;
}
bool VulkanHardwareBuffer::recordSampling(VkCommandBuffer command, VkImage output,
        VkImageView outputView, uint32_t queueFamily, VkImageLayout outputLayout,
        TransferCurve curve) {
    if (!pipeline || !command || !output || !outputView ||
        queueFamily == VK_QUEUE_FAMILY_IGNORED) return false;
    if (!mDescriptorPool) {
        // YCbCr combined samplers may consume several hardware descriptors.
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}
        };
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(mDevice->device, &pool, nullptr,
                &mDescriptorPool) != VK_SUCCESS) return false;
    }
    if (!mDescriptorSet) {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = mDescriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptorLayout;
        if (vkAllocateDescriptorSets(mDevice->device, &allocate,
                &mDescriptorSet) != VK_SUCCESS) return false;
    }
    VkDescriptorImageInfo source{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo target{VK_NULL_HANDLE, outputView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = writes[1].dstSet = mDescriptorSet;
    writes[0].dstBinding = 0; writes[1].dstBinding = 1;
    writes[0].descriptorCount = writes[1].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &source; writes[1].pImageInfo = &target;
    vkUpdateDescriptorSets(mDevice->device, 2, writes, 0, nullptr);
    VkImageMemoryBarrier acquire[2]{};
    for (auto& barrier : acquire) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    acquire[0].image = image;
    acquire[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    acquire[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    acquire[0].dstQueueFamilyIndex = queueFamily;
    acquire[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    acquire[1].image = output;
    acquire[1].oldLayout = outputLayout;
    acquire[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire[1].srcQueueFamilyIndex = acquire[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquire[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, acquire);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &mDescriptorSet, 0, nullptr);
    const uint32_t transfer = static_cast<uint32_t>(curve);
    vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(transfer), &transfer);
    vkCmdDispatch(command, (width + 15) / 16, (height + 15) / 16, 1);
    VkImageMemoryBarrier release[2] = {acquire[0], acquire[1]};
    release[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    release[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release[0].srcQueueFamilyIndex = queueFamily;
    release[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    release[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    release[0].dstAccessMask = 0;
    release[1].oldLayout = release[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    release[1].dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, release);
    return true;
}
bool VulkanHardwareBuffer::submitSampling(VkImage output, VkImageView outputView,
        VkImageLayout outputLayout, int acquireFenceFd,
        VkSemaphore outputAvailable, VkSemaphore* outputReady, int* releaseFd,
        TransferCurve curve) {
    const auto closeAcquire = [&] { if (acquireFenceFd >= 0) ::close(acquireFenceFd); };
    if (!mDevice || !pipeline || !outputReady || !releaseFd ||
        !mDevice->queue_compute.count) { closeAcquire(); return false; }
    *outputReady = VK_NULL_HANDLE;
    *releaseFd = -1;
    const uint32_t family = mDevice->queue_compute.index;
    if (mSubmitted) {
        // The caller has also completed libplacebo's wait on mOutputReady.
        if (vkWaitForFences(mDevice->device, 1, &mCompletion, VK_TRUE,
                std::numeric_limits<uint64_t>::max()) != VK_SUCCESS) {
            closeAcquire();
            return false;
        }
        mSubmitted = false;
    }
    if (mAcquire) {
        vkDestroySemaphore(mDevice->device, mAcquire, nullptr);
        mAcquire = VK_NULL_HANDLE;
    }
    if (mCommandPool && vkResetCommandPool(mDevice->device, mCommandPool, 0) != VK_SUCCESS) {
        closeAcquire();
        return false;
    }
    if (mCompletion && vkResetFences(mDevice->device, 1, &mCompletion) != VK_SUCCESS) {
        closeAcquire();
        return false;
    }
    if (acquireFenceFd >= 0) {
        mAcquire = importAndroidAcquireFence(mDevice, acquireFenceFd);
        if (!mAcquire) return false;
    }
    if (!mOutputReady) mOutputReady = createAndroidReleaseSemaphore(mDevice);
    if (!mInputReleased) mInputReleased = createAndroidReleaseSemaphore(mDevice);
    if (!mOutputReady || !mInputReleased) return false;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = family;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (!mCommandPool && vkCreateCommandPool(mDevice->device, &pool, nullptr, &mCommandPool) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = mCommandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!mCommand && vkAllocateCommandBuffers(mDevice->device, &allocate, &mCommand) != VK_SUCCESS)
        return false;
    const VkCommandBuffer command = mCommand;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS ||
        !recordSampling(command, output, outputView, family, outputLayout, curve) ||
        vkEndCommandBuffer(command) != VK_SUCCESS) return false;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!mCompletion && vkCreateFence(mDevice->device, &fence, nullptr, &mCompletion) != VK_SUCCESS)
        return false;
    VkSemaphore waits[2]{};
    VkPipelineStageFlags stages[2] = {
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    uint32_t waitCount = 0;
    if (mAcquire) waits[waitCount++] = mAcquire;
    if (outputAvailable) waits[waitCount++] = outputAvailable;
    VkSemaphore signals[] = {mOutputReady, mInputReleased};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 2;
    submit.pSignalSemaphores = signals;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(mDevice->device, family, 0, &queue);
    mDevice->lock_queue(mDevice, family, 0);
    const VkResult result = vkQueueSubmit(queue, 1, &submit, mCompletion);
    mDevice->unlock_queue(mDevice, family, 0);
    if (result != VK_SUCCESS) return false;
    mSubmitted = true;
    bool exported = false;
    *releaseFd = exportAndroidReleaseFence(mDevice, mInputReleased, &exported);
    if (!exported) {
        // Exceptional fallback only: the caller may now safely delete AImage.
        vkWaitForFences(mDevice->device, 1, &mCompletion, VK_TRUE,
            std::numeric_limits<uint64_t>::max());
        return false;
    }
    *outputReady = mOutputReady;
    return true;
}
bool VulkanHardwareBuffer::initialize(pl_vulkan device, AHardwareBuffer* buffer,
        bool fullRange, bool smoothChroma) {
    if (mDevice || !device || !buffer) return false;
    auto propertiesFn = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        device->get_proc_addr(device->instance, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!propertiesFn) return false;
    VkAndroidHardwareBufferFormatPropertiesANDROID format{};
    format.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID properties{};
    properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    properties.pNext = &format;
    if (propertiesFn(device->device, buffer, &properties) != VK_SUCCESS ||
        !(format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !format.externalFormat) return false;
    // A defined native VkFormat does not preclude external-format sampling.
    // Keep one import path and use the external format's queried capabilities.
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    if (!desc.width || !desc.height || desc.layers != 1 ||
        !(desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)) return false;
    mDevice = device;
    mBuffer = buffer;
    AHardwareBuffer_acquire(buffer);
    width = desc.width; height = desc.height;
    externalFormat = format.externalFormat;
    VkExternalFormatANDROID external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    external.externalFormat = externalFormat;
    VkExternalMemoryImageCreateInfo externalMemory{};
    externalMemory.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemory.pNext = &external;
    externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.pNext = &externalMemory;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = VK_FORMAT_UNDEFINED;
    create.extent = {width, height, 1};
    create.mipLevels = 1; create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device->device, &create, nullptr, &image) != VK_SUCCESS) return false;
    // AHB allocationSize / memoryTypeBits are authoritative. Querying image
    // requirements before binding imported AHB memory is prohibited by Vulkan.
    uint32_t memoryType = 0;
    while (memoryType < 32 && !(properties.memoryTypeBits & (1u << memoryType))) ++memoryType;
    if (memoryType == 32) return false;
    VkImportAndroidHardwareBufferInfoANDROID imported{};
    imported.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    imported.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &imported;
    dedicated.image = image;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &dedicated;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device->device, &allocation, nullptr, &mMemory) != VK_SUCCESS ||
        vkBindImageMemory(device->device, image, mMemory, 0) != VK_SUCCESS) return false;
    VkSamplerYcbcrConversionCreateInfo ycbcr{};
    ycbcr.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    ycbcr.pNext = &external;
    ycbcr.format = create.format;
    ycbcr.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
    ycbcr.ycbcrRange = fullRange ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                                : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    // Component swizzles are ignored for external-format conversion.
    ycbcr.xChromaOffset = format.suggestedXChromaOffset;
    ycbcr.yChromaOffset = format.suggestedYChromaOffset;
    const VkFormatFeatureFlags linearFlags =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    ycbcr.chromaFilter = (smoothChroma && (format.formatFeatures & linearFlags) == linearFlags)
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if (vkCreateSamplerYcbcrConversion(device->device, &ycbcr, nullptr, &conversion) != VK_SUCCESS)
        return false;
    VkSamplerYcbcrConversionInfo conversionInfo{};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversionInfo.conversion = conversion;
    VkImageViewCreateInfo imageView{};
    imageView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageView.pNext = &conversionInfo;
    imageView.image = image;
    imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageView.format = create.format;
    imageView.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device->device, &imageView, nullptr, &view) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sample{};
    sample.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sample.pNext = &conversionInfo;
    sample.magFilter = sample.minFilter = ycbcr.chromaFilter;
    sample.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sample.addressModeU = sample.addressModeV = sample.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device->device, &sample, nullptr, &sampler) != VK_SUCCESS) return false;
    spdlog::info("GALLERY_PERF event=mediacodec_vulkan_buffer_import width={} height={} external_format={} model={} range={}",
        width, height, externalFormat, int(ycbcr.ycbcrModel), int(ycbcr.ycbcrRange));
    return createSamplingPipeline();
}
}
#endif

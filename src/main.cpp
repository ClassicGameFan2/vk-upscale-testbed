#define _CRT_SECURE_NO_WARNINGS
#define VOLK_IMPLEMENTATION
#include "volk.h"

#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <iostream>
#include <vector>
#include <malloc.h>
#include <string>

// Image libs
#include "stb_image.h"
#include "stb_image_write.h"

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        char buffer[2048];
        size_t converted = 0;
        wcstombs_s(&converted, buffer, sizeof(buffer), message, _TRUNCATE);
        std::cout << "[AMD SDK] " << buffer << std::endl;
    }
}

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0; 
}

// --- VULKAN HELPERS FOR IMAGE UPLOAD / DOWNLOAD ---
void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, properties);
    vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory);
    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

VkImageCreateInfo createImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& imageMemory) {
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imageInfo, nullptr, &image);

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory);
    vkBindImageMemory(device, image, imageMemory, 0);
    return imageInfo;
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspectMask) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT; destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "Usage: vk-upscale-testbed.exe <input.png> <output.png> <scale_factor>" << std::endl;
        return 1;
    }

    std::string inFile = argv[1];
    std::string outFile = argv[2];
    float scaleFactor = std::stof(argv[3]);

    std::cout << "--- Vulkan Temporal I/O Testbed ---" << std::endl;
    std::cout << "Loading " << inFile << "..." << std::endl;

    int renderW, renderH, channels;
    unsigned char* pixels = stbi_load(inFile.c_str(), &renderW, &renderH, &channels, 4);
    if (!pixels) {
        std::cout << "FAILED: Could not load " << inFile << ". Does the file exist?" << std::endl;
        return 1;
    }

    uint32_t displayW = (uint32_t)(renderW * scaleFactor);
    uint32_t displayH = (uint32_t)(renderH * scaleFactor);
    std::cout << "Scaling: " << renderW << "x" << renderH << " -> " << displayW << "x" << displayH << std::endl;

    std::cout << "Initializing Volk..." << std::endl;
    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "FAILED: volkInitialize() failed. Make sure vulkan-1.dll is in your Working Directory!" << std::endl;
        return 1;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3; 
    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &appInfo;
    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "FAILED: vkCreateInstance failed!" << std::endl;
        return 1;
    }
    volkLoadInstance(instance);

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::cout << "FAILED: No Vulkan physical devices found!" << std::endl;
        return 1;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = physicalDevices[0]; 

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    int queueFamilyIndex = -1;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            queueFamilyIndex = i; break;
        }
    }
    if (queueFamilyIndex == -1) {
        std::cout << "FAILED: No graphics/compute queue found!" << std::endl;
        return 1;
    }

    // Shotgun all extensions for compatibility with FSR 2
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, availableExts.data());
    std::vector<const char*> enabledExtensions;
    for (const auto& ext : availableExts) {
        enabledExtensions.push_back(ext.extensionName);
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
    VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceInfo.pNext = &features2; 
    deviceInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        std::cout << "FAILED: vkCreateDevice failed!" << std::endl;
        return 1;
    }
    volkLoadDevice(device);

    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; 
    VkCommandPool commandPool;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);

    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    std::cout << "Allocating VRAM Buffers..." << std::endl;

    // --- ALLOCATE IMAGES & STAGING BUFFERS ---
    VkDeviceSize imageSize = renderW * renderH * 4;
    VkBuffer uploadBuffer; VkDeviceMemory uploadMemory;
    createBuffer(device, physicalDevice, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uploadBuffer, uploadMemory);
    
    void* data;
    vkMapMemory(device, uploadMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, uploadMemory);
    stbi_image_free(pixels);

    VkBuffer downloadBuffer; VkDeviceMemory downloadMemory;
    VkDeviceSize outSize = displayW * displayH * 4;
    createBuffer(device, physicalDevice, outSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, downloadBuffer, downloadMemory);

    VkImage colorImage, depthImage, mvImage, outputImage;
    VkDeviceMemory colorMem, depthMem, mvMem, outputMem;
    
    VkImageCreateInfo colorInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, colorImage, colorMem);
    VkImageCreateInfo depthInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, depthImage, depthMem);
    VkImageCreateInfo mvInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, mvImage, mvMem);
    VkImageCreateInfo outputInfo = createImage(device, physicalDevice, displayW, displayH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, outputImage, outputMem);

    // --- SETUP FSR 2 CONTEXT ---
    std::cout << "Creating FSR 2.3.3 Context..." << std::endl;
    size_t safeBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 1) * 2;
    void* scratchBuffer = _aligned_malloc(safeBufferSize, 64);

    VkDeviceContext vkDeviceContext = { device, physicalDevice, vkGetDeviceProcAddr };
    FfxInterface ffxInterface = {};
    ffxGetInterfaceVK(&ffxInterface, ffxGetDeviceVK(&vkDeviceContext), scratchBuffer, safeBufferSize, 1);

    FfxFsr2ContextDescription fsr2Desc = {};
    fsr2Desc.flags = FFX_FSR2_ENABLE_DEBUG_CHECKING; // Clean configuration for Static SDR Images
    fsr2Desc.maxRenderSize = { (uint32_t)renderW, (uint32_t)renderH };
    fsr2Desc.displaySize = { displayW, displayH };
    fsr2Desc.fpMessage = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface;

    FfxFsr2Context fsr2Context;
    if (ffxFsr2ContextCreate(&fsr2Context, &fsr2Desc) != FFX_OK) {
        std::cout << "FAILED: ffxFsr2ContextCreate failed!" << std::endl;
        return 1;
    }

    // --- RECORD PRE-LOOP UPLOADS ---
    std::cout << "Uploading Image to VRAM..." << std::endl;
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition & Clear Inputs
    transitionImageLayout(cmd, colorImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transitionImageLayout(cmd, depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    transitionImageLayout(cmd, mvImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transitionImageLayout(cmd, outputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

    // Upload Color
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
    vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Clear Depth to 1.0f (Standard Far Plane)
    VkClearDepthStencilValue depthClear = {1.0f, 0}; 
    VkImageSubresourceRange depthRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depthClear, 1, &depthRange);

    VkClearColorValue mvClear = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange mvRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mvClear, 1, &mvRange);

    // Transition to FSR Expected Layouts
    transitionImageLayout(cmd, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transitionImageLayout(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    transitionImageLayout(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // --- TEMPORAL LOOP ---
    std::cout << "Dispatching FSR 2.3.3 Static Frame Loop (32 Passes)..." << std::endl;

    FfxResource colorRes = ffxGetResourceVK(colorImage, ffxGetImageResourceDescriptionVK(colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes = ffxGetResourceVK(depthImage, ffxGetImageResourceDescriptionVK(depthImage, depthInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes = ffxGetResourceVK(mvImage, ffxGetImageResourceDescriptionVK(mvImage, mvInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"MVs", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes = ffxGetResourceVK(outputImage, ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV), L"Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    for (int i = 0; i < 32; i++) {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        FfxFsr2DispatchDescription dispatchDesc = {};
        dispatchDesc.commandList = ffxGetCommandListVK(cmd);
        dispatchDesc.color = colorRes;
        dispatchDesc.depth = depthRes;
        dispatchDesc.motionVectors = mvRes;
        dispatchDesc.output = outputRes;
        
        // Zero Jitter for Flat Static Sprites
        dispatchDesc.jitterOffset.x = 0.0f;
        dispatchDesc.jitterOffset.y = 0.0f;
        dispatchDesc.motionVectorScale.x = 0.0f; 
        dispatchDesc.motionVectorScale.y = 0.0f;
        
        dispatchDesc.renderSize = { (uint32_t)renderW, (uint32_t)renderH };
        dispatchDesc.enableSharpening = true;
        dispatchDesc.sharpness = 0.2f;
        dispatchDesc.frameTimeDelta = 16.6f;
        dispatchDesc.preExposure = 1.0f;
        dispatchDesc.reset = (i == 0); 
        
        // Standard Depth Math (No NaN explosion)
        dispatchDesc.cameraNear = 0.1f;
        dispatchDesc.cameraFar = 100.0f;
        dispatchDesc.cameraFovAngleVertical = 1.047f; // ~60 degrees
        dispatchDesc.viewSpaceToMetersFactor = 1.0f;

        if (ffxFsr2ContextDispatch(&fsr2Context, &dispatchDesc) != FFX_OK) {
            std::cout << "FAILED: ffxFsr2ContextDispatch failed on loop " << i << std::endl;
            return 1;
        }

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        if ((i + 1) % 8 == 0) std::cout << "  -> Completed Pass " << (i + 1) << "/32" << std::endl;
    }

    // --- DOWNLOAD RESULT ---
    std::cout << "Downloading Result..." << std::endl;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transitionImageLayout(cmd, outputImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy outRegion = {};
    outRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outRegion.imageSubresource.layerCount = 1;
    outRegion.imageExtent = { displayW, displayH, 1 };
    
    vkCmdCopyImageToBuffer(cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);

    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // --- SAVE PNG ---
    void* outData;
    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    stbi_write_png(outFile.c_str(), displayW, displayH, 4, outData, displayW * 4);
    vkUnmapMemory(device, downloadMemory);
    std::cout << "Saved " << outFile << " successfully!" << std::endl;

    // Cleanup
    ffxFsr2ContextDestroy(&fsr2Context);
    vkDestroyImage(device, colorImage, nullptr); vkFreeMemory(device, colorMem, nullptr);
    vkDestroyImage(device, depthImage, nullptr); vkFreeMemory(device, depthMem, nullptr);
    vkDestroyImage(device, mvImage, nullptr); vkFreeMemory(device, mvMem, nullptr);
    vkDestroyImage(device, outputImage, nullptr); vkFreeMemory(device, outputMem, nullptr);
    vkDestroyBuffer(device, uploadBuffer, nullptr); vkFreeMemory(device, uploadMemory, nullptr);
    vkDestroyBuffer(device, downloadBuffer, nullptr); vkFreeMemory(device, downloadMemory, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    _aligned_free(scratchBuffer);

    return 0;
}

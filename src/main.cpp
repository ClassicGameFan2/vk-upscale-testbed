#define VOLK_IMPLEMENTATION
#include "volk.h"

#include <FidelityFX/host/ffx_fsr1.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <iostream>
#include <vector>
#include <malloc.h>

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0; 
}

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    void destroy(VkDevice device) {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

Texture createTexture(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t width, uint32_t height, 
                      VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Texture tex;
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
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
    vkCreateImage(device, &imageInfo, nullptr, &tex.image);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, tex.image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &tex.memory);
    vkBindImageMemory(device, tex.image, tex.memory, 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &viewInfo, nullptr, &tex.view);

    return tex;
}

int main() {
    std::cout << "--- Vulkan Headless Testbed (FSR 1.0 via SDK v1.1.4) ---" << std::endl;
    
    if (volkInitialize() != VK_SUCCESS) return 1;

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VkUpscaleTestbed";
    appInfo.apiVersion = VK_API_VERSION_1_3; 

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return 1;

    volkLoadInstance(instance);

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
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
            queueFamilyIndex = i;
            break;
        }
    }

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, availableExts.data());
    
    std::vector<const char*> enabledExtensions;
    for (const auto& ext : availableExts) {
        enabledExtensions.push_back(ext.extensionName);
    }

    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;

    VkPhysicalDeviceVulkan11Features features11 = {};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.pNext = &features12;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features11;
    
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceInfo.pNext = &features2; 
    deviceInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) return 1;
    
    volkLoadDevice(device);

    std::cout << "SUCCESS: Core Vulkan 1.3 Initialized with " << enabledExtensions.size() << " Extensions." << std::endl;

    uint32_t renderW = 320, renderH = 240;
    uint32_t displayW = 640, displayH = 480;

    // =========================================================================
    // PHASE 3: INITIALIZING THE AMD FIDELITYFX FSR 1.0 SDK
    // =========================================================================
    std::cout << "\nInitializing AMD FSR 1.0 Context..." << std::endl;

    size_t scratchBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 1);
    void* scratchBuffer = _aligned_malloc(scratchBufferSize, 64);

    VkDeviceContext vkDeviceContext = {};
    vkDeviceContext.vkDevice = device;
    vkDeviceContext.vkPhysicalDevice = physicalDevice;
    vkDeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    
    FfxDevice ffxDevice = ffxGetDeviceVK(&vkDeviceContext);

    FfxInterface ffxInterface = {};
    FfxErrorCode err = ffxGetInterfaceVK(&ffxInterface, ffxDevice, scratchBuffer, scratchBufferSize, 1);
    
    if (err != FFX_OK) {
        std::cout << "FAILED: Could not establish AMD FFX Interface!" << std::endl;
        return 1;
    }

    // COMPLIANT FSR 1.0 CONTEXT SETUP
    FfxFsr1ContextDescription fsr1Desc = {};
    fsr1Desc.flags = FFX_FSR1_ENABLE_RCAS;
    // CRITICAL: We must explicitly declare the output format for FSR 1.0!
    fsr1Desc.outputFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    fsr1Desc.maxRenderSize.width = renderW;
    fsr1Desc.maxRenderSize.height = renderH;
    fsr1Desc.displaySize.width = displayW;
    fsr1Desc.displaySize.height = displayH;
    fsr1Desc.backendInterface = ffxInterface;

    std::cout << " -> ffxFsr1ContextCreate..." << std::endl;
    FfxFsr1Context fsr1Context;
    err = ffxFsr1ContextCreate(&fsr1Context, &fsr1Desc);
    std::cout << " -> ffxFsr1ContextCreate Finished. Result Code: " << err << std::endl;
    
    if (err != FFX_OK) {
        std::cout << "FAILED: SwiftShader rejected the FSR 1.0 Context!" << std::endl;
    } else {
        std::cout << "=========================================================" << std::endl;
        std::cout << "SUCCESS: AMD FSR 1.0 UPSCALER IS ALIVE ON CPU!!!" << std::endl;
        std::cout << "=========================================================" << std::endl;
        
        ffxFsr1ContextDestroy(&fsr1Context);
    }

    // =========================================================================
    // CLEANUP
    // =========================================================================
    _aligned_free(scratchBuffer);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    
    std::cout << "--- Phase 3 shut down cleanly. ---" << std::endl;

    return 0;
}

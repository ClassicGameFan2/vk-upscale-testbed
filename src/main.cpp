#define VOLK_IMPLEMENTATION
#include "volk.h"

// CRITICAL FIX: The officially supported way to use VMA + Volk. 
// By keeping Static ON, VMA's code gets replaced by Volk's safe macros during compilation!
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1002000
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <iostream>
#include <vector>

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;

    void destroy(VkDevice device, VmaAllocator allocator) {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image && allocation) vmaDestroyImage(allocator, image, allocation);
    }
};

Texture createTexture(VkDevice device, VmaAllocator allocator, uint32_t width, uint32_t height, 
                      VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, const std::string& name) {
    Texture tex;
    std::cout << "  -> Creating Image Info for " << name << "..." << std::endl;

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

    // DIAGNOSTIC TEST: Can SwiftShader natively create the Image Handle?
    std::cout << "  -> [RAW VULKAN TEST] vkCreateImage... " << std::flush;
    VkImage testImage;
    if (vkCreateImage(device, &imageInfo, nullptr, &testImage) != VK_SUCCESS) {
        std::cout << "FAILED! SwiftShader rejected the image format/usage." << std::endl;
        return tex;
    }
    std::cout << "SUCCESS!" << std::endl;
    vkDestroyImage(device, testImage, nullptr);

    // If raw Vulkan succeeds, try VMA memory mapping
    std::cout << "  -> [VMA TEST] vmaCreateImage... " << std::flush;
    VmaAllocationCreateInfo allocInfo = {};
    // Fall back to the absolute safest memory request for a CPU emulator
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU; 

    if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &tex.image, &tex.allocation, nullptr) != VK_SUCCESS) {
        std::cout << "FAILED! VMA crashed during memory allocation." << std::endl;
        return tex;
    }
    std::cout << "SUCCESS!" << std::endl;

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

    if (vkCreateImageView(device, &viewInfo, nullptr, &tex.view) != VK_SUCCESS) {
        std::cout << "  -> FAILED at vkCreateImageView!" << std::endl;
        return tex;
    }

    std::cout << "  -> " << name << " Allocation Complete!" << std::endl;
    return tex;
}

int main() {
    std::cout << "--- Vulkan Headless Testbed (Phase 2 - Diagnostic) ---" << std::endl;

    if (volkInitialize() != VK_SUCCESS) return 1;

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VkUpscaleTestbed";
    appInfo.apiVersion = VK_API_VERSION_1_2; 

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

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) return 1;
    volkLoadDevice(device);

    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;

    VmaAllocator allocator;
    if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) return 1;
    std::cout << "SUCCESS: Core Vulkan Initialized." << std::endl;

    uint32_t renderW = 320, renderH = 240;
    uint32_t displayW = 640, displayH = 480;

    std::cout << "\nAllocating VRAM Textures..." << std::endl;

    Texture colorTex = createTexture(device, allocator, renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, 
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "Color Input");

    Texture depthTex = createTexture(device, allocator, renderW, renderH, VK_FORMAT_D32_SFLOAT, 
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "Depth Buffer");

    Texture motionTex = createTexture(device, allocator, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, 
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "Motion Vectors");

    Texture outputTex = createTexture(device, allocator, displayW, displayH, VK_FORMAT_R8G8B8A8_UNORM, 
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "Output Color");

    std::cout << "\nSUCCESS: 4x FSR Textures Allocated in VRAM." << std::endl;

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool commandPool;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) return 1;

    VkCommandBufferAllocateInfo allocCmdInfo = {};
    allocCmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocCmdInfo.commandPool = commandPool;
    allocCmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCmdInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(device, &allocCmdInfo, &commandBuffer) != VK_SUCCESS) return 1;

    colorTex.destroy(device, allocator);
    depthTex.destroy(device, allocator);
    motionTex.destroy(device, allocator);
    outputTex.destroy(device, allocator);

    vkDestroyCommandPool(device, commandPool, nullptr);
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    
    std::cout << "--- Phase 2 shut down cleanly. ---" << std::endl;

    return 0;
}

#define VOLK_IMPLEMENTATION
#include "volk.h"

// Initialize the AMD Vulkan Memory Allocator
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <iostream>
#include <vector>
#include <stdexcept>

int main() {
    std::cout << "--- Vulkan Headless Testbed (Phase 1) ---" << std::endl;

    // 1. Initialize Volk (Finds vulkan-1.dll / SwiftShader)
    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "ERROR: Failed to load Vulkan driver!" << std::endl;
        return 1;
    }

    // 2. Create the Vulkan Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VkUpscaleTestbed";
    appInfo.apiVersion = VK_API_VERSION_1_2; // FSR 2/3 requires Vulkan 1.2+

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "ERROR: Failed to create Vulkan Instance!" << std::endl;
        return 1;
    }
    volkLoadInstance(instance);
    std::cout << "SUCCESS: Vulkan Instance Created." << std::endl;

    // 3. Find the Physical Device (SwiftShader)
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

    VkPhysicalDevice physicalDevice = physicalDevices[0]; // Just grab the first one (SwiftShader)
    
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProps);
    std::cout << "SUCCESS: Connected to GPU -> " << deviceProps.deviceName << std::endl;

    // 4. Find the Graphics & Compute Queue Family
    // GPUs have different "Queues" for different math. We need one that can do Graphics AND Compute.
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    int queueFamilyIndex = -1;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && 
            (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            queueFamilyIndex = i;
            break;
        }
    }

    if (queueFamilyIndex == -1) {
        std::cout << "ERROR: Could not find a Graphics/Compute queue!" << std::endl;
        return 1;
    }

    // 5. Create the Logical Device (The software bridge to the GPU)
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
    
    // We will enable FSR-specific Vulkan extensions here later!
    deviceInfo.enabledExtensionCount = 0; 
    deviceInfo.ppEnabledExtensionNames = nullptr;

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        std::cout << "ERROR: Failed to create Logical Device!" << std::endl;
        return 1;
    }
    volkLoadDevice(device);
    std::cout << "SUCCESS: Logical Device Created." << std::endl;

    // Retrieve the actual Queue handle so we can submit commands to it later
    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    // 6. Initialize AMD Vulkan Memory Allocator (VMA)
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;

    VmaAllocator allocator;
    if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) {
        std::cout << "ERROR: Failed to initialize AMD VMA!" << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: AMD Vulkan Memory Allocator Initialized." << std::endl;

    // 7. Clean up memory to prevent leaks
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    
    std::cout << "--- Testbed shut down cleanly. Ready for Phase 2! ---" << std::endl;

    return 0;
}

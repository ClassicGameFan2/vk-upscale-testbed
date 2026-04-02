#define VOLK_IMPLEMENTATION
#include "volk.h"

// CRITICAL FIX: Tell VMA we are compiling against Vulkan 1.2+ core functions!
// This stops VMA from trying to call ancient NULL-pointer KHR extensions and crashing.
#define VMA_VULKAN_VERSION 1002000
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <iostream>
#include <vector>

int main() {
    std::cout << "--- Vulkan Headless Testbed (Phase 1) ---" << std::endl;

    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "ERROR: Failed to load Vulkan driver!" << std::endl;
        return 1;
    }

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

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

    VkPhysicalDevice physicalDevice = physicalDevices[0]; 
    
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProps);
    std::cout << "SUCCESS: Connected to GPU -> " << deviceProps.deviceName << std::endl;

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
    deviceInfo.enabledExtensionCount = 0; 
    deviceInfo.ppEnabledExtensionNames = nullptr;

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        std::cout << "ERROR: Failed to create Logical Device!" << std::endl;
        return 1;
    }
    volkLoadDevice(device);
    std::cout << "SUCCESS: Logical Device Created." << std::endl;

    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    // Initialize AMD Vulkan Memory Allocator (VMA)
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

    // Clean up
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    
    std::cout << "--- Testbed shut down cleanly. Ready for Phase 2! ---" << std::endl;

    return 0;
}

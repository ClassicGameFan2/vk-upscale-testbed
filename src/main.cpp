#define VOLK_IMPLEMENTATION
#include "volk.h"
#include <iostream>
#include <vector>

int main() {
    std::cout << "--- Vulkan Headless Testbed ---" << std::endl;

    // 1. Initialize Volk (Dynamically searches for vulkan-1.dll)
    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "ERROR: Failed to find a Vulkan driver (vulkan-1.dll)!" << std::endl;
        std::cout << "Please place Google's SwiftShader 'vulkan-1.dll' in the same folder as this .exe." << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: Vulkan API loaded via Volk!" << std::endl;

    // 2. Create the Vulkan Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VkUpscaleTestbed";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2; // FSR 2.2 explicitly requires Vulkan 1.2

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "ERROR: Failed to create Vulkan Instance!" << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: Vulkan Instance created." << std::endl;

    // Load instance-based Vulkan functions
    volkLoadInstance(instance);

    // 3. Enumerate Physical Devices (Find our "Graphics Card")
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        std::cout << "ERROR: Failed to find any GPUs with Vulkan support!" << std::endl;
        return 1;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    std::cout << "\nDetected Vulkan Devices:" << std::endl;
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);
        std::cout << " -> " << deviceProperties.deviceName << " (API Version: " 
                  << VK_VERSION_MAJOR(deviceProperties.apiVersion) << "." 
                  << VK_VERSION_MINOR(deviceProperties.apiVersion) << ")" << std::endl;
    }

    // 4. Clean up memory
    vkDestroyInstance(instance, nullptr);
    std::cout << "\nTestbed initialized and shut down cleanly." << std::endl;

    return 0;
}

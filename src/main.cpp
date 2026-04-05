#define VOLK_IMPLEMENTATION
#include "volk.h"

#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <iostream>
#include <vector>
#include <malloc.h>

// --- VULKAN VALIDATION LAYER CALLBACK ---
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cout << "\n[VULKAN VALIDATION] " << pCallbackData->pMessage << std::endl;
    }
    return VK_FALSE;
}

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        char buffer[2048];
        size_t converted = 0;
        wcstombs_s(&converted, buffer, sizeof(buffer), message, _TRUNCATE);
        std::cout << "[AMD SDK LOG] " << buffer << std::endl;
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

int main() {
    std::cout << "--- Vulkan Headless Testbed (FSR 2.3.3 + Validation Layers) ---" << std::endl;

    if (volkInitialize() != VK_SUCCESS) return 1;

    // 1. Enable Validation Layers
    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    bool validationFound = false;
    for (const auto& layer : availableLayers) {
        if (strcmp(layer.layerName, validationLayerName) == 0) {
            validationFound = true;
            break;
        }
    }

    std::vector<const char*> enabledLayers;
    std::vector<const char*> instanceExtensions;

    if (validationFound) {
        enabledLayers.push_back(validationLayerName);
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        std::cout << "Validation Layers Enabled." << std::endl;
    } else {
        std::cout << "Validation Layers Not Found. Running blind." << std::endl;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VkUpscaleTestbed";
    appInfo.apiVersion = VK_API_VERSION_1_3; 

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = (uint32_t)enabledLayers.size();
    instanceInfo.ppEnabledLayerNames = enabledLayers.data();
    instanceInfo.enabledExtensionCount = (uint32_t)instanceExtensions.size();
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return 1;

    volkLoadInstance(instance);

    // 2. Attach the Debug Messenger
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    if (validationFound) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo = {};
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = debugCallback;

        auto createDebug = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (createDebug) createDebug(instance, &debugInfo, nullptr, &debugMessenger);
    }

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

    std::cout << "\nInitializing AMD FSR 2.3.3 Context..." << std::endl;

    size_t scratchBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 1);
    
    // Double the buffer size and align it to 64 bytes to guarantee no memory bounds errors
    size_t safeBufferSize = scratchBufferSize * 2;
    void* scratchBuffer = _aligned_malloc(safeBufferSize, 64);

    VkDeviceContext vkDeviceContext = {};
    vkDeviceContext.vkDevice = device;
    vkDeviceContext.vkPhysicalDevice = physicalDevice;
    vkDeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    FfxDevice ffxDevice = ffxGetDeviceVK(&vkDeviceContext);

    FfxInterface ffxInterface = {};
    FfxErrorCode err = ffxGetInterfaceVK(&ffxInterface, ffxDevice, scratchBuffer, safeBufferSize, 1);
    if (err != FFX_OK) {
        std::cout << "FAILED: Could not establish AMD FFX Interface!" << std::endl;
        return 1;
    }

    FfxFsr2ContextDescription fsr2Desc = {};
    fsr2Desc.flags = FFX_FSR2_ENABLE_DEBUG_CHECKING | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    fsr2Desc.maxRenderSize.width = renderW;
    fsr2Desc.maxRenderSize.height = renderH;
    fsr2Desc.displaySize.width = displayW;
    fsr2Desc.displaySize.height = displayH;
    fsr2Desc.fpMessage = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface;

    std::cout << "  -> ffxFsr2ContextCreate..." << std::endl;
    FfxFsr2Context fsr2Context;
    err = ffxFsr2ContextCreate(&fsr2Context, &fsr2Desc);
    std::cout << "  -> ffxFsr2ContextCreate Finished. Result Code: " << err << std::endl;

    if (err != FFX_OK) {
        std::cout << "FAILED: SwiftShader rejected the FSR 2.3.3 Context!" << std::endl;
    } else {
        std::cout << "=========================================================" << std::endl;
        std::cout << "SUCCESS: AMD FSR 2.3.3 TEMPORAL UPSCALER IS ALIVE ON CPU!!!" << std::endl;
        std::cout << "=========================================================" << std::endl;
        ffxFsr2ContextDestroy(&fsr2Context);
    }

    _aligned_free(scratchBuffer);

    vkDestroyDevice(device, nullptr);

    if (debugMessenger) {
        auto destroyDebug = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyDebug) destroyDebug(instance, debugMessenger, nullptr);
    }

    vkDestroyInstance(instance, nullptr);
    std::cout << "--- Phase 3 shut down cleanly. ---" << std::endl;
    return 0;
}

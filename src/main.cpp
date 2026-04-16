#define _CRT_SECURE_NO_WARNINGS
#define VOLK_IMPLEMENTATION
#include "common.h"

// Forward declarations of pass functions
void RunFsr1Pass(
    VkDevice, VkPhysicalDevice, VkQueue, VkCommandBuffer,
    VkBuffer, VkDeviceMemory, VkBuffer, VkDeviceMemory,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkDeviceSize, VkDeviceSize,
    VkDeviceContext&, void*, size_t);

void RunFsr2Pass(
    VkDevice, VkPhysicalDevice, VkQueue, VkCommandBuffer,
    VkBuffer, VkDeviceMemory, VkBuffer, VkDeviceMemory,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkImage,
    VkDeviceSize, VkDeviceSize, VkDeviceSize, VkDeviceSize,
    VkDeviceContext&, void*, size_t);

void RunFsr3Pass(
    VkDevice, VkPhysicalDevice, VkQueue, VkCommandBuffer,
    VkBuffer, VkDeviceMemory, VkBuffer, VkDeviceMemory,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkImage, VkImageCreateInfo,
    VkDeviceSize, VkDeviceSize, VkDeviceSize, VkDeviceSize,
    VkDeviceContext&, void*, size_t);

static void AssertCallback(const char* message) {
    std::cout << "\n[AMD SDK ASSERT] " << message << "\n";
    std::cout.flush();
}

int main()
{
    std::cout << "============================================\n";
    std::cout << "  FSR Vulkan Testbed\n";
    std::cout << "============================================\n";
    std::cout.flush();

    ffxAssertSetPrintingCallback(AssertCallback);

    // ── Vulkan instance ───────────────────────────────────────────────────
    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "[FATAL] volk init failed\n"; return 1;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceCI = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceCI.pApplicationInfo = &appInfo;

    VkInstance instance;
    if (vkCreateInstance(&instanceCI, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateInstance failed\n"; return 1;
    }
    volkLoadInstance(instance);
    std::cout << "[Init] Vulkan instance OK.\n";

    // ── Physical device ───────────────────────────────────────────────────
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());
    VkPhysicalDevice physDev = physDevs[0];

    VkPhysicalDeviceProperties devProps;
    vkGetPhysicalDeviceProperties(physDev, &devProps);
    std::cout << "[Init] GPU: " << devProps.deviceName << "\n";

    // ── Logical device ────────────────────────────────────────────────────
    float queuePri = 1.f;
    VkDeviceQueueCreateInfo queueCI = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCI.queueFamilyIndex = 0;
    queueCI.queueCount       = 1;
    queueCI.pQueuePriorities = &queuePri;

    VkPhysicalDeviceVulkan12Features features12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
    VkPhysicalDeviceFeatures2 features2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
    vkGetPhysicalDeviceFeatures2(physDev, &features2);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &extCount, exts.data());
    std::vector<const char*> enabledExts;
    for (auto& e : exts) enabledExts.push_back(e.extensionName);

    VkDeviceCreateInfo deviceCI = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCI.queueCreateInfoCount    = 1;
    deviceCI.pQueueCreateInfos       = &queueCI;
    deviceCI.pNext                   = &features2;
    deviceCI.enabledExtensionCount   = (uint32_t)enabledExts.size();
    deviceCI.ppEnabledExtensionNames = enabledExts.data();

    VkDevice device;
    if (vkCreateDevice(physDev, &deviceCI, nullptr, &device) != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateDevice failed\n"; return 1;
    }
    volkLoadDevice(device);
    std::cout << "[Init] Logical device OK.\n";

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // ── Command pool / buffer ─────────────────────────────────────────────
    VkCommandPoolCreateInfo poolCI = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.queueFamilyIndex = 0;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool;
    vkCreateCommandPool(device, &poolCI, nullptr, &commandPool);

    VkCommandBufferAllocateInfo cmdAlloc = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAlloc.commandPool        = commandPool;
    cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

    // ── CPU reference renders ─────────────────────────────────────────────
    std::cout << "\n[CPU] Rendering Native_1x.png...\n";
    {
        std::vector<float> c(RENDER_W * RENDER_H * 4);
        std::vector<float> d(RENDER_W * RENDER_H);
        std::vector<float> m(RENDER_W * RENDER_H * 2, 0.f);
        renderScene(RENDER_W, RENDER_H, 0.f, 0.f, 0.f, 0.f,
                    c.data(), d.data(), m.data());
        saveFloatImage("Native_1x.png", RENDER_W, RENDER_H, c.data());
        saveDepthImage("Native_1x_depth.png", RENDER_W, RENDER_H, d.data());
    }
    {
        std::vector<float> c(DISPLAY_W * DISPLAY_H * 4);
        std::vector<float> d(DISPLAY_W * DISPLAY_H);
        std::vector<float> m(DISPLAY_W * DISPLAY_H * 2, 0.f);
        renderScene(DISPLAY_W, DISPLAY_H, 0.f, 0.f, 0.f, 0.f,
                    c.data(), d.data(), m.data());
        saveFloatImage("Native_2x.png", DISPLAY_W, DISPLAY_H, c.data());
    }

    // ── Shared Vulkan resources ───────────────────────────────────────────
    VkDeviceSize colorUploadSize = RENDER_W * RENDER_H * 4 * sizeof(float);
    VkDeviceSize depthUploadSize = RENDER_W * RENDER_H * 1 * sizeof(float);
    VkDeviceSize mvUploadSize    = RENDER_W * RENDER_H * 2 * sizeof(float);
    VkDeviceSize uploadSize      = colorUploadSize + depthUploadSize + mvUploadSize;
    VkDeviceSize outSize         = DISPLAY_W * DISPLAY_H * 4 * sizeof(float);

    VkBuffer      uploadBuffer;  VkDeviceMemory uploadMemory;
    VkBuffer      downloadBuffer; VkDeviceMemory downloadMemory;
    createBuffer(device, physDev, uploadSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 uploadBuffer, uploadMemory);
    createBuffer(device, physDev, outSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 downloadBuffer, downloadMemory);

    VkImage colorImage;  VkDeviceMemory colorMem;
    VkImage depthImage;  VkDeviceMemory depthMem;
    VkImage mvImage;     VkDeviceMemory mvMem;
    VkImage outputImage; VkDeviceMemory outputMem;
    VkImage expImage;    VkDeviceMemory expMem;

    VkImageCreateInfo colorInfo = createImage(device, physDev,
        RENDER_W, RENDER_H, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        colorImage, colorMem);

    VkImageCreateInfo depthInfo = createImage(device, physDev,
        RENDER_W, RENDER_H, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        depthImage, depthMem);

    VkImageCreateInfo mvInfo = createImage(device, physDev,
        RENDER_W, RENDER_H, VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        mvImage, mvMem);

    VkImageCreateInfo outputInfo = createImage(device, physDev,
        DISPLAY_W, DISPLAY_H, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT      | VK_IMAGE_USAGE_SAMPLED_BIT,
        outputImage, outputMem);

    // expImage is used by FSR2 only
    VkImageCreateInfo expInfo = createImage(device, physDev,
        1, 1, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        expImage, expMem);

    VkDeviceContext vkDevCtx = { device, physDev, vkGetDeviceProcAddr };

    size_t scratchSize =
        ffxGetScratchMemorySizeVK(physDev, 4) * 2;

    void* scratch1 = _aligned_malloc(scratchSize, 64);
    void* scratch2 = _aligned_malloc(scratchSize, 64);
    void* scratch3 = _aligned_malloc(scratchSize, 64);

    // ── FSR 1.2 ───────────────────────────────────────────────────────────
    RunFsr1Pass(
        device, physDev, queue, cmd,
        uploadBuffer, uploadMemory,
        downloadBuffer, downloadMemory,
        colorImage,  colorInfo,
        outputImage, outputInfo,
        colorUploadSize, outSize,
        vkDevCtx, scratch1, scratchSize);

    // ── FSR 2.3.3 ─────────────────────────────────────────────────────────
    RunFsr2Pass(
        device, physDev, queue, cmd,
        uploadBuffer, uploadMemory,
        downloadBuffer, downloadMemory,
        colorImage, colorInfo,
        depthImage, depthInfo,
        mvImage,    mvInfo,
        outputImage, outputInfo,
        expImage,
        colorUploadSize, depthUploadSize, mvUploadSize, outSize,
        vkDevCtx, scratch2, scratchSize);

    // ── FSR 3.1.4 ─────────────────────────────────────────────────────────
    RunFsr3Pass(
        device, physDev, queue, cmd,
        uploadBuffer, uploadMemory,
        downloadBuffer, downloadMemory,
        colorImage, colorInfo,
        depthImage, depthInfo,
        mvImage,    mvInfo,
        outputImage, outputInfo,
        colorUploadSize, depthUploadSize, mvUploadSize, outSize,
        vkDevCtx, scratch3, scratchSize);

    // ── Cleanup ───────────────────────────────────────────────────────────
    vkDestroyImage  (device, colorImage,    nullptr);
    vkFreeMemory    (device, colorMem,      nullptr);
    vkDestroyImage  (device, depthImage,    nullptr);
    vkFreeMemory    (device, depthMem,      nullptr);
    vkDestroyImage  (device, mvImage,       nullptr);
    vkFreeMemory    (device, mvMem,         nullptr);
    vkDestroyImage  (device, outputImage,   nullptr);
    vkFreeMemory    (device, outputMem,     nullptr);
    vkDestroyImage  (device, expImage,      nullptr);
    vkFreeMemory    (device, expMem,        nullptr);
    vkDestroyBuffer (device, uploadBuffer,  nullptr);
    vkFreeMemory    (device, uploadMemory,  nullptr);
    vkDestroyBuffer (device, downloadBuffer, nullptr);
    vkFreeMemory    (device, downloadMemory, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice (device, nullptr);
    vkDestroyInstance(instance, nullptr);
    _aligned_free(scratch1);
    _aligned_free(scratch2);
    _aligned_free(scratch3);

    std::cout << "\n[Done] All tests complete.\n";
    return 0;
}

#define _CRT_SECURE_NO_WARNINGS
#define VOLK_IMPLEMENTATION
#include "volk.h"
#include <FidelityFX/host/ffx_fsr1.h>
#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <iostream>
#include <vector>
#include <malloc.h>
#include <string>
#include <cmath>

#include "stb_image_write.h"

// =============================================================================
// AMD SDK CALLBACKS
// =============================================================================
static void AssertCallback(const char* message) {
    std::cout << "\n[AMD SDK ASSERTION FAILED] " << message << std::endl;
    std::cout.flush();
}

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        std::cout << "[AMD SDK MSG] ";
        for (int i = 0; i < 2048 && message[i] != 0; ++i) {
            std::cout << (char)message[i];
        }
        std::cout << std::endl;
        std::cout.flush();
    }
}

// =============================================================================
// VULKAN HELPERS
// =============================================================================
static uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return 0;
}

void createBuffer(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& bufferMemory)
{
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physicalDevice, memRequirements.memoryTypeBits, properties);
    vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory);
    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

VkImageCreateInfo createImage(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& imageMemory)
{
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType         = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width      = width;
    imageInfo.extent.height     = height;
    imageInfo.extent.depth      = 1;
    imageInfo.mipLevels         = 1;
    imageInfo.arrayLayers       = 1;
    imageInfo.format            = format;
    imageInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage             = usage;
    imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imageInfo, nullptr, &image);

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize  = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physicalDevice,
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory);
    vkBindImageMemory(device, image, imageMemory, 0);

    return imageInfo;
}

void transition(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageLayout& currentLayout,
    VkImageLayout newLayout,
    VkImageAspectFlags aspectMask)
{
    if (currentLayout == newLayout) return;

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout                       = currentLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = aspectMask;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    currentLayout = newLayout;
}

// =============================================================================
// PROCEDURAL 3D RAYTRACER
// Renders HDR linear floats with inverted depth (1=near, 0=far)
// Scene: Red sphere on checkered floor under blue sky
// =============================================================================
void renderScene(
    int w, int h,
    float jx, float jy,
    float* colorOut,
    float* depthOut)
{
    float aspect     = (float)w / (float)h;
    float fovY       = 1.04719755f; // 60 degrees
    float tanHalfFov = tanf(fovY / 2.0f);
    float zNear      = 0.1f;
    float zFar       = 100.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float ndcX = ((x + 0.5f + jx) / w) * 2.0f - 1.0f;
            float ndcY = ((y + 0.5f + jy) / h) * 2.0f - 1.0f;

            float ro[3] = { 0.0f, 1.0f, 0.0f };
            float rd[3] = {
                ndcX * aspect * tanHalfFov,
                -ndcY * tanHalfFov,
                1.0f
            };
            float len = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0] /= len; rd[1] /= len; rd[2] /= len;

            float hitZ = -1.0f;
            // Sky color (blue)
            float r = powf(135.0f/255.0f, 2.2f);
            float g = powf(206.0f/255.0f, 2.2f);
            float b = powf(235.0f/255.0f, 2.2f);

            // --- Sphere intersection ---
            float oc[3] = {
                ro[0] - 0.0f,
                ro[1] - 1.0f,
                ro[2] - 4.0f
            };
            float b_dot = rd[0]*oc[0] + rd[1]*oc[1] + rd[2]*oc[2];
            float c_val = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - (1.5f * 1.5f);
            float disc  = b_dot*b_dot - c_val;

            if (disc > 0.0f) {
                float t = -b_dot - sqrtf(disc);
                if (t > zNear && t < zFar) {
                    hitZ = t;
                    float nx = (ro[0] + rd[0]*t) - 0.0f;
                    float ny = (ro[1] + rd[1]*t) - 1.0f;
                    float nz = (ro[2] + rd[2]*t) - 4.0f;
                    float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
                    nx /= nlen; ny /= nlen; nz /= nlen;

                    float light[3] = { 0.577f, 0.577f, -0.577f };
                    float ndotl = fmaxf(0.2f,
                        -(nx*light[0] + ny*light[1] + nz*light[2]));
                    // Red sphere color
                    r = powf(200.0f/255.0f, 2.2f) * ndotl;
                    g = powf(50.0f/255.0f,  2.2f) * ndotl;
                    b = powf(50.0f/255.0f,  2.2f) * ndotl;
                }
            }

            // --- Floor intersection ---
            if (rd[1] < 0.0f) {
                float t = -ro[1] / rd[1];
                if (t > zNear && t < zFar && (hitZ < 0.0f || t < hitZ)) {
                    hitZ = t;
                    float px = ro[0] + rd[0]*t;
                    float pz = ro[2] + rd[2]*t;
                    int chk = ((int)floorf(px) + (int)floorf(pz)) % 2;
                    if (chk == 0) {
                        r = powf(220.0f/255.0f, 2.2f);
                        g = r; b = r;
                    } else {
                        r = powf(80.0f/255.0f, 2.2f);
                        g = r; b = r;
                    }
                }
            }

            int idx = y * w + x;
            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 1.0f;

            // Inverted depth: near=1.0, far=0.0
            if (hitZ > 0.0f) {
                float depth = (zNear * (zFar - hitZ)) /
                              (hitZ  * (zFar - zNear));
                depthOut[idx] = depth;
            } else {
                depthOut[idx] = 0.0f;
            }
        }
    }
}

// =============================================================================
// IMAGE SAVE HELPER
// Converts linear HDR floats to gamma-corrected 8-bit PNG
// =============================================================================
void saveFloatImage(
    const std::string& filename,
    int w, int h,
    const float* data)
{
    std::vector<unsigned char> bytes(w * h * 4);
    int nanCount = 0;

    for (int i = 0; i < w * h * 4; i += 4) {
        for (int c = 0; c < 3; c++) {
            float v = data[i+c];
            if (std::isnan(v)) {
                v = 0.0f;
                nanCount++;
            }
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            bytes[i+c] = (unsigned char)(powf(v, 1.0f / 2.2f) * 255.0f);
        }
        bytes[i+3] = 255;
    }

    if (nanCount > 0) {
        std::cout << "[WARNING] " << nanCount
                  << " NaN pixels detected in " << filename << "!" << std::endl;
        std::cout.flush();
    }

    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w * 4);
    std::cout << "Saved: " << filename << std::endl;
    std::cout.flush();
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "  FSR Vulkan Testbed - SwiftShader Edition  " << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout.flush();

    ffxAssertSetPrintingCallback(AssertCallback);

    // Resolution: render at 320x240, display at 640x480 (2x upscale)
    const uint32_t renderW  = 320;
    const uint32_t renderH  = 240;
    const uint32_t displayW = 640;
    const uint32_t displayH = 480;

    // =========================================================================
    // VULKAN INSTANCE
    // =========================================================================
    std::cout << "\n[Init] Creating Vulkan instance..." << std::endl;
    std::cout.flush();

    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "[FATAL] volkInitialize failed!" << std::endl;
        return 1;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateInstance failed!" << std::endl;
        return 1;
    }
    volkLoadInstance(instance);
    std::cout << "[Init] Vulkan instance created OK." << std::endl;
    std::cout.flush();

    // =========================================================================
    // PHYSICAL DEVICE
    // =========================================================================
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = physicalDevices[0];

    VkPhysicalDeviceProperties devProps;
    vkGetPhysicalDeviceProperties(physicalDevice, &devProps);
    std::cout << "[Init] Using GPU: " << devProps.deviceName << std::endl;
    std::cout.flush();

    // =========================================================================
    // LOGICAL DEVICE
    // =========================================================================
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, queueFamilies.data());

    int queueFamilyIndex = 0;
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount       = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    // Query all supported features and pass them back - enables everything
    // SwiftShader advertises without us manually picking features
    VkPhysicalDeviceVulkan12Features features12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
    VkPhysicalDeviceFeatures2 features2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    // IMPORTANT: Force shaderFloat16 OFF at device creation level.
    // This is an extra safety net on top of the CMake FP16 kill switch.
    // SwiftShader advertises FP16 support but its math is broken for
    // negative numbers (corrupts YCoCg blue/red channels -> dark silhouette).
    std::cout << "[Init] Forcing shaderFloat16 = FALSE (SwiftShader FP16 bug workaround)"
              << std::endl;
    std::cout.flush();
    features12.shaderFloat16 = VK_FALSE;
    features11.storageBuffer16BitAccess = VK_FALSE;

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &extCount, availableExts.data());
    std::vector<const char*> enabledExtensions;
    for (const auto& ext : availableExts) {
        enabledExtensions.push_back(ext.extensionName);
    }

    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount     = 1;
    deviceInfo.pQueueCreateInfos        = &queueCreateInfo;
    deviceInfo.pNext                    = &features2;
    deviceInfo.enabledExtensionCount    = (uint32_t)enabledExtensions.size();
    deviceInfo.ppEnabledExtensionNames  = enabledExtensions.data();

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateDevice failed!" << std::endl;
        return 1;
    }
    volkLoadDevice(device);
    std::cout << "[Init] Logical device created OK." << std::endl;
    std::cout.flush();

    // =========================================================================
    // COMMAND POOL AND BUFFER
    // =========================================================================
    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);

    VkCommandBufferAllocateInfo cmdAllocInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAllocInfo.commandPool        = commandPool;
    cmdAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submitInfo = {};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    // =========================================================================
    // RENDER NATIVE REFERENCE IMAGES (CPU raytracer - no upscaling)
    // =========================================================================
    std::cout << "\n[CPU] Rendering Native_1x.png (320x240, no upscaling)..."
              << std::endl;
    std::cout.flush();
    std::vector<float> native1x(renderW * renderH * 4);
    std::vector<float> depth1x(renderW * renderH);
    renderScene(renderW, renderH, 0.0f, 0.0f, native1x.data(), depth1x.data());
    saveFloatImage("Native_1x.png", renderW, renderH, native1x.data());

    std::cout << "[CPU] Rendering Native_2x.png (640x480, no upscaling)..."
              << std::endl;
    std::cout.flush();
    std::vector<float> native2x(displayW * displayH * 4);
    std::vector<float> depth2x(displayW * displayH);
    renderScene(displayW, displayH, 0.0f, 0.0f, native2x.data(), depth2x.data());
    saveFloatImage("Native_2x.png", displayW, displayH, native2x.data());

    // =========================================================================
    // SHARED VULKAN RESOURCES
    // =========================================================================

    // Upload buffer (CPU -> GPU): holds one frame of color + depth
    VkDeviceSize uploadSize =
        (renderW * renderH * 4 * sizeof(float)) +
        (renderW * renderH * sizeof(float));
    VkBuffer      uploadBuffer; VkDeviceMemory uploadMemory;
    createBuffer(device, physicalDevice, uploadSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        uploadBuffer, uploadMemory);

    // Download buffer (GPU -> CPU): holds one upscaled frame
    VkDeviceSize  outSize = displayW * displayH * 4 * sizeof(float);
    VkBuffer      downloadBuffer; VkDeviceMemory downloadMemory;
    createBuffer(device, physicalDevice, outSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        downloadBuffer, downloadMemory);

    // GPU images
    VkImage       colorImage,  depthImage,  mvImage,  outputImage,  expImage;
    VkDeviceMemory colorMem,   depthMem,    mvMem,    outputMem,    expImageMem;

    // Color input: R32G32B32A32 (full 128-bit float, no precision loss)
    VkImageCreateInfo colorInfo = createImage(
        device, physicalDevice, renderW, renderH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        colorImage, colorMem);

    // Depth input: D32_SFLOAT (inverted, 1=near, 0=far)
    VkImageCreateInfo depthInfo = createImage(
        device, physicalDevice, renderW, renderH,
        VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        depthImage, depthMem);

    // Motion vectors: R32G32 (zero = static scene)
    VkImageCreateInfo mvInfo = createImage(
        device, physicalDevice, renderW, renderH,
        VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        mvImage, mvMem);

    // Output image: R32G32B32A32 (FSR writes upscaled result here)
    VkImageCreateInfo outputInfo = createImage(
        device, physicalDevice, displayW, displayH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT      |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        outputImage, outputMem);

    // Exposure texture: 1x1 R32_SFLOAT
    // We manually clear this to 1.0f every frame (no auto-exposure).
    // Auto-exposure is DISABLED because FSR2's SPD compute shader
    // requires subgroupSize >= 32, but SwiftShader only has size 4.
    // This causes NaN cascade which makes the entire image black.
    VkImageCreateInfo expInfo = createImage(
        device, physicalDevice, 1, 1,
        VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        expImage, expImageMem);

    // Image layout tracking
    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout expLayout    = VK_IMAGE_LAYOUT_UNDEFINED;

    // FSR backend scratch memory
    VkDeviceContext vkDeviceContext = { device, physicalDevice, vkGetDeviceProcAddr };
    size_t safeBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 4) * 2;
    void* scratchBuffer = _aligned_malloc(safeBufferSize, 64);
    if (!scratchBuffer) {
        std::cout << "[FATAL] Failed to allocate scratch buffer!" << std::endl;
        return 1;
    }

    // Subresource range helpers
    VkImageSubresourceRange colorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // =========================================================================
    // FSR 1.2 SPATIAL UPSCALING TEST
    // Simple single-pass spatial upscaler - reference for correct colors
    // =========================================================================
    std::cout << "\n============================================" << std::endl;
    std::cout << "  FSR 1.2 Spatial Upscaling Test            " << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout.flush();

    memset(scratchBuffer, 0, safeBufferSize);

    FfxInterface ffxInterface1 = {};
    if (ffxGetInterfaceVK(
            &ffxInterface1,
            ffxGetDeviceVK(&vkDeviceContext),
            scratchBuffer, safeBufferSize, 4) != FFX_OK)
    {
        std::cout << "[FATAL] ffxGetInterfaceVK failed for FSR1!" << std::endl;
        return 1;
    }

    FfxFsr1ContextDescription fsr1Desc = {};
    fsr1Desc.flags =
        FFX_FSR1_ENABLE_HIGH_DYNAMIC_RANGE; // HDR on, RCAS off for clean test
    fsr1Desc.outputFormat  = ffxGetSurfaceFormatVK(VK_FORMAT_R32G32B32A32_SFLOAT);
    fsr1Desc.maxRenderSize = { renderW, renderH };
    fsr1Desc.displaySize   = { displayW, displayH };
    fsr1Desc.backendInterface = ffxInterface1;

    FfxFsr1Context* fsr1Context =
        (FfxFsr1Context*)_aligned_malloc(sizeof(FfxFsr1Context), 64);
    memset(fsr1Context, 0, sizeof(FfxFsr1Context));
    if (ffxFsr1ContextCreate(fsr1Context, &fsr1Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr1ContextCreate failed!" << std::endl;
        return 1;
    }

    FfxResource colorRes1 = ffxGetResourceVK(
        colorImage,
        ffxGetImageResourceDescriptionVK(
            colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR1_Color",
        FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes1 = ffxGetResourceVK(
        outputImage,
        ffxGetImageResourceDescriptionVK(
            outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV),
        L"FSR1_Output",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    // Upload frame 0 color to GPU
    void* mappedData;
    vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
    memcpy(mappedData, native1x.data(), native1x.size() * sizeof(float));
    vkUnmapMemory(device, uploadMemory);

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transition(cmd, colorImage,  colorLayout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,    VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_GENERAL,                 VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy cRegion = {};
    cRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cRegion.imageSubresource.layerCount = 1;
    cRegion.imageExtent = { renderW, renderH, 1 };
    vkCmdCopyBufferToImage(
        cmd, uploadBuffer, colorImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion);

    transition(cmd, colorImage, colorLayout,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    FfxFsr1DispatchDescription dispatchDesc1 = {};
    dispatchDesc1.commandList      = ffxGetCommandListVK(cmd);
    dispatchDesc1.color            = colorRes1;
    dispatchDesc1.output           = outputRes1;
    dispatchDesc1.renderSize       = { renderW, renderH };
    dispatchDesc1.enableSharpening = false; // RCAS off for clean baseline
    dispatchDesc1.sharpness        = 0.0f;

    std::cout << "[FSR1] Dispatching FSR 1.2 upscaler..." << std::endl;
    std::cout.flush();
    if (ffxFsr1ContextDispatch(fsr1Context, &dispatchDesc1) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr1ContextDispatch failed!" << std::endl;
        return 1;
    }

    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy outRegion = {};
    outRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outRegion.imageSubresource.layerCount = 1;
    outRegion.imageExtent = { displayW, displayH, 1 };
    vkCmdCopyImageToBuffer(
        cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        downloadBuffer, 1, &outRegion);

    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* outData;
    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_1.2_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr1ContextDestroy(fsr1Context);
    _aligned_free(fsr1Context);
    std::cout << "[FSR1] Done." << std::endl;
    std::cout.flush();

    // =========================================================================
    // FSR 2.3.3 TEMPORAL UPSCALING TEST
    //
    // SwiftShader workarounds active:
    //   [A] shaderFloat16 = FALSE at device creation (above)
    //   [B] fp16Supported = false in ffx_vk.cpp (CMake Patch-C)
    //   [C] B10G11R11 -> R16G16B16A16 format redirect (CMake Patch-D)
    //   [D] Manual exposure = 1.0f (bypasses SPD subgroup-size crash)
    //   [E] FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE = OFF
    //       (HDR mode uses YCoCg internally which has negative values.
    //        Even with FP32, the tonemapping curve may interact badly
    //        with SwiftShader. SDR mode is simpler and safer.)
    // =========================================================================
    std::cout << "\n============================================" << std::endl;
    std::cout << "  FSR 2.3.3 Temporal Upscaling Test (32 frames)" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout.flush();

    memset(scratchBuffer, 0, safeBufferSize);

    FfxInterface ffxInterface2 = {};
    if (ffxGetInterfaceVK(
            &ffxInterface2,
            ffxGetDeviceVK(&vkDeviceContext),
            scratchBuffer, safeBufferSize, 4) != FFX_OK)
    {
        std::cout << "[FATAL] ffxGetInterfaceVK failed for FSR2!" << std::endl;
        return 1;
    }

    FfxFsr2ContextDescription fsr2Desc = {};
    memset(&fsr2Desc, 0, sizeof(FfxFsr2ContextDescription));

    // CRITICAL FLAG DECISIONS FOR SWIFTSHADER:
    // - FFX_FSR2_ENABLE_DEBUG_CHECKING : ON  (see AMD SDK messages)
    // - FFX_FSR2_ENABLE_DEPTH_INVERTED : ON  (our depth is 1=near, 0=far)
    // - FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE : OFF
    //     WHY: HDR mode converts colors to YCoCg which has negative chroma.
    //     Even with FP32 math, this is an unnecessary risk on SwiftShader.
    //     Our scene looks correct in SDR linear space.
    // - FFX_FSR2_ENABLE_AUTO_EXPOSURE : OFF (NOT set)
    //     WHY: Auto-exposure uses SPD which requires subgroupSize >= 32.
    //     SwiftShader has subgroupSize = 4. This causes NaN -> black screen.
    //     We provide manual exposure = 1.0f via expImage instead.
    fsr2Desc.flags =
        FFX_FSR2_ENABLE_DEBUG_CHECKING  |
        FFX_FSR2_ENABLE_DEPTH_INVERTED;
    // Note: HDR and AUTO_EXPOSURE deliberately NOT set

    fsr2Desc.maxRenderSize    = { renderW, renderH };
    fsr2Desc.displaySize      = { displayW, displayH };
    fsr2Desc.fpMessage        = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface2;

    std::cout << "[FSR2] Creating FSR2 context..." << std::endl;
    std::cout.flush();

    FfxFsr2Context* fsr2Context =
        (FfxFsr2Context*)_aligned_malloc(sizeof(FfxFsr2Context), 64);
    memset(fsr2Context, 0, sizeof(FfxFsr2Context));
    if (ffxFsr2ContextCreate(fsr2Context, &fsr2Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr2ContextCreate failed!" << std::endl;
        return 1;
    }
    std::cout << "[FSR2] Context created OK." << std::endl;
    std::cout.flush();

    // Build FfxResource descriptors (reused every frame)
    FfxResource colorRes2 = ffxGetResourceVK(
        colorImage,
        ffxGetImageResourceDescriptionVK(
            colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_Color",
        FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes2 = ffxGetResourceVK(
        depthImage,
        ffxGetImageResourceDescriptionVK(
            depthImage, depthInfo, FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_Depth",
        FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes2 = ffxGetResourceVK(
        mvImage,
        ffxGetImageResourceDescriptionVK(
            mvImage, mvInfo, FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_MVs",
        FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes2 = ffxGetResourceVK(
        outputImage,
        ffxGetImageResourceDescriptionVK(
            outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV),
        L"FSR2_Output",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    FfxResource expRes2 = ffxGetResourceVK(
        expImage,
        ffxGetImageResourceDescriptionVK(
            expImage, expInfo, FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_Exposure",
        FFX_RESOURCE_STATE_COMPUTE_READ);

    int32_t phaseCount = ffxFsr2GetJitterPhaseCount(renderW, displayW);
    std::cout << "[FSR2] Jitter phase count: " << phaseCount << std::endl;
    std::cout << "[FSR2] Running 32 temporal accumulation frames..." << std::endl;
    std::cout.flush();

    for (int i = 0; i < 32; i++) {
        // Get jitter offset for this frame
        float jX = 0.0f, jY = 0.0f;
        ffxFsr2GetJitterOffset(&jX, &jY, i, phaseCount);

        // Render jittered frame on CPU
        std::vector<float> fColor(renderW * renderH * 4);
        std::vector<float> fDepth(renderW * renderH);
        renderScene(renderW, renderH, jX, jY, fColor.data(), fDepth.data());

        // Upload color + depth to GPU via staging buffer
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
        memcpy(mappedData,
            fColor.data(),
            fColor.size() * sizeof(float));
        memcpy((uint8_t*)mappedData + (fColor.size() * sizeof(float)),
            fDepth.data(),
            fDepth.size() * sizeof(float));
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Transition all inputs to TRANSFER_DST for uploads/clears
        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // On frame 0, transition output to GENERAL (FSR2 writes to it)
        if (i == 0) {
            transition(cmd, outputImage, outputLayout,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        // Copy color buffer
        VkBufferImageCopy cRegion2 = {};
        cRegion2.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cRegion2.imageSubresource.layerCount = 1;
        cRegion2.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(
            cmd, uploadBuffer, colorImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion2);

        // Copy depth buffer
        VkBufferImageCopy dRegion = {};
        dRegion.bufferOffset = fColor.size() * sizeof(float);
        dRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dRegion.imageSubresource.layerCount = 1;
        dRegion.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(
            cmd, uploadBuffer, depthImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dRegion);

        // Clear motion vectors to 0 (static scene, no motion)
        VkClearColorValue mvClear = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(
            cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &mvClear, 1, &colorRange);

        // Clear exposure to 1.0f (manual exposure, bypasses SPD crash)
        VkClearColorValue expClear = {{ 1.0f, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(
            cmd, expImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &expClear, 1, &colorRange);

        // Transition all inputs to SHADER_READ_ONLY for FSR2
        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // Build dispatch description
        FfxFsr2DispatchDescription dispatchDesc = {};
        memset(&dispatchDesc, 0, sizeof(FfxFsr2DispatchDescription));

        dispatchDesc.commandList    = ffxGetCommandListVK(cmd);
        dispatchDesc.color          = colorRes2;
        dispatchDesc.depth          = depthRes2;
        dispatchDesc.motionVectors  = mvRes2;
        dispatchDesc.exposure       = expRes2;   // Manual 1.0f exposure
        dispatchDesc.output         = outputRes2;

        // Jitter offset from FSR2's Halton sequence
        dispatchDesc.jitterOffset.x = jX;
        dispatchDesc.jitterOffset.y = jY;

        // Motion vector scale: screen-space pixels
        // Our motion vectors are in pixel space [-(w/2)..(w/2), -(h/2)..(h/2)]
        dispatchDesc.motionVectorScale.x = (float)renderW;
        dispatchDesc.motionVectorScale.y = (float)renderH;

        dispatchDesc.renderSize          = { renderW, renderH };
        dispatchDesc.enableSharpening    = false; // RCAS off until colors correct
        dispatchDesc.sharpness           = 0.0f;
        dispatchDesc.frameTimeDelta      = 16.6f; // ~60fps
        dispatchDesc.preExposure         = 1.0f;
        dispatchDesc.reset               = (i == 0); // Clear history on frame 0
        dispatchDesc.cameraNear          = 0.1f;
        dispatchDesc.cameraFar           = 100.0f;
        dispatchDesc.cameraFovAngleVertical = 1.047f; // 60 degrees
        dispatchDesc.viewSpaceToMetersFactor = 1.0f;

        if (ffxFsr2ContextDispatch(fsr2Context, &dispatchDesc) != FFX_OK) {
            std::cout << "[FATAL] ffxFsr2ContextDispatch failed on frame "
                      << i << "!" << std::endl;
            return 1;
        }

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        if ((i + 1) % 8 == 0) {
            std::cout << "[FSR2] Completed frame " << (i+1) << "/32" << std::endl;
            std::cout.flush();
        }
    }

    // Download final upscaled frame from GPU
    std::cout << "[FSR2] Downloading result..." << std::endl;
    std::cout.flush();

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);
    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImageToBuffer(
        cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        downloadBuffer, 1, &outRegion);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_2.3.3_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr2ContextDestroy(fsr2Context);
    _aligned_free(fsr2Context);
    std::cout << "[FSR2] Done." << std::endl;
    std::cout.flush();

    // =========================================================================
    // CLEANUP
    // =========================================================================
    std::cout << "\n[Cleanup] Destroying Vulkan resources..." << std::endl;
    std::cout.flush();

    vkDestroyImage(device, colorImage,   nullptr); vkFreeMemory(device, colorMem,     nullptr);
    vkDestroyImage(device, depthImage,   nullptr); vkFreeMemory(device, depthMem,     nullptr);
    vkDestroyImage(device, mvImage,      nullptr); vkFreeMemory(device, mvMem,        nullptr);
    vkDestroyImage(device, outputImage,  nullptr); vkFreeMemory(device, outputMem,    nullptr);
    vkDestroyImage(device, expImage,     nullptr); vkFreeMemory(device, expImageMem,  nullptr);
    vkDestroyBuffer(device, uploadBuffer,   nullptr); vkFreeMemory(device, uploadMemory,   nullptr);
    vkDestroyBuffer(device, downloadBuffer, nullptr); vkFreeMemory(device, downloadMemory, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    _aligned_free(scratchBuffer);

    std::cout << "[Done] All tests complete. Check PNG files in build directory."
              << std::endl;
    std::cout.flush();
    return 0;
}

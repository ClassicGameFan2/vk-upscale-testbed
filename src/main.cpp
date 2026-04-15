#define _CRT_SECURE_NO_WARNINGS
#define VOLK_IMPLEMENTATION
#include "volk.h"
#include <FidelityFX/host/ffx_fsr1.h>
#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <iostream>
#include <vector>
#include <malloc.h>
#include <string>
#include <cmath>
#include <cfloat>

#include "stb_image_write.h"

static void AssertCallback(const char* message) {
    std::cout << "\n[AMD SDK ASSERT] " << message << std::endl;
    std::cout.flush();
}

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        std::cout << "[AMD MSG] ";
        for (int i = 0; i < 2048 && message[i] != 0; ++i)
            std::cout << (char)message[i];
        std::cout << std::endl;
        std::cout.flush();
    }
}

static void Fsr3MessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        std::cout << "[FSR3 MSG] ";
        for (int i = 0; i < 2048 && message[i] != 0; ++i)
            std::cout << (char)message[i];
        std::cout << std::endl;
        std::cout.flush();
    }
}

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
    uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return 0;
}

void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buffer, &memReq);
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice,
        memReq.memoryTypeBits, properties);
    vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory);
    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

VkImageCreateInfo createImage(VkDevice device, VkPhysicalDevice physicalDevice,
    uint32_t width, uint32_t height, VkFormat format,
    VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& imageMemory) {
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imageInfo, nullptr, &image);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, image, &memReq);
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice,
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory);
    vkBindImageMemory(device, image, imageMemory, 0);
    return imageInfo;
}

// Create a VkImage from an FfxCreateResourceDescription.
// Maps FfxSurfaceFormat -> VkFormat and FfxResourceUsage -> VkImageUsageFlags.
static VkFormat ffxFormatToVk(FfxSurfaceFormat fmt) {
    switch (fmt) {
    case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32B32A32_UINT:    return VK_FORMAT_R32G32B32A32_UINT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32_FLOAT:         return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32_TYPELESS:      return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R8_UINT:              return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R32_UINT:             return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT:      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT:         return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_UINT:          return VK_FORMAT_R16G16_UINT;
    case FFX_SURFACE_FORMAT_R16G16_SINT:          return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT:            return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16_UINT:             return VK_FORMAT_R16_UINT;
    case FFX_SURFACE_FORMAT_R16_UNORM:            return VK_FORMAT_R16_UNORM;
    case FFX_SURFACE_FORMAT_R16_SNORM:            return VK_FORMAT_R16_SNORM;
    case FFX_SURFACE_FORMAT_R8_UNORM:             return VK_FORMAT_R8_UNORM;
    case FFX_SURFACE_FORMAT_R32_FLOAT:            return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R8G8_UNORM:           return VK_FORMAT_R8G8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8_UINT:            return VK_FORMAT_R8G8_UINT;
    case FFX_SURFACE_FORMAT_R32G32B32_FLOAT:      return VK_FORMAT_R32G32B32_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_TYPELESS:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM:    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:   return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default:
        std::cout << "[WARN] Unknown FfxSurfaceFormat " << (int)fmt
                  << ", falling back to R32_SFLOAT" << std::endl;
        return VK_FORMAT_R32_SFLOAT;
    }
}

static VkImageUsageFlags ffxUsageToVk(FfxResourceUsage usage) {
    VkImageUsageFlags flags =
        VK_IMAGE_USAGE_SAMPLED_BIT |        // always SRV-able
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |   // for clears/uploads
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;    // for readback
    if (usage & FFX_RESOURCE_USAGE_UAV)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & FFX_RESOURCE_USAGE_RENDERTARGET)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return flags;
}

struct SharedImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = {};
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

static SharedImage createSharedImage(VkDevice device, VkPhysicalDevice physicalDevice,
    const FfxCreateResourceDescription& desc)
{
    SharedImage si;
    VkFormat fmt = ffxFormatToVk(desc.resourceDescription.format);
    VkImageUsageFlags usage = ffxUsageToVk(desc.resourceDescription.usage);

    uint32_t w = desc.resourceDescription.width;
    uint32_t h = desc.resourceDescription.height;
    uint32_t mips = desc.resourceDescription.mipCount;
    if (mips == 0) {
        mips = 1;
        uint32_t tw = w, th = h;
        while (tw > 1 || th > 1) { tw >>= 1; th >>= 1; mips++; }
    }

    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType   = VK_IMAGE_TYPE_2D;
    imageInfo.extent      = { w, h, 1 };
    imageInfo.mipLevels   = mips;
    imageInfo.arrayLayers = 1;
    imageInfo.format      = fmt;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage       = usage;
    imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateImage(device, &imageInfo, nullptr, &si.image);

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, si.image, &memReq);
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice,
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &si.memory);
    vkBindImageMemory(device, si.image, si.memory, 0);

    si.info   = imageInfo;
    si.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return si;
}

void transition(VkCommandBuffer cmd, VkImage image,
    VkImageLayout& currentLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspectMask, uint32_t mipLevels = 1) {
    if (currentLayout == newLayout) return;
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = currentLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspectMask, 0, mipLevels, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    currentLayout = newLayout;
}

void renderScene(int w, int h,
    float currJX, float currJY,
    float prevJX, float prevJY,
    float* colorOut, float* depthOut, float* mvOut) {
    float aspect = (float)w / (float)h;
    float fovY = 1.04719755f;
    float tanHalfFov = tanf(fovY / 2.0f);
    float zNear = 0.1f, zFar = 100.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float ndcX = ((x + 0.5f + currJX) / w) * 2.0f - 1.0f;
            float ndcY = ((y + 0.5f + currJY) / h) * 2.0f - 1.0f;
            float ro[3] = { 0.0f, 1.0f, 0.0f };
            float rd[3] = { ndcX * aspect * tanHalfFov,
                           -ndcY * tanHalfFov, 1.0f };
            float len = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0]/=len; rd[1]/=len; rd[2]/=len;

            float hitZ = -1.0f;
            float r = powf(135.0f/255.0f, 2.2f);
            float g = powf(206.0f/255.0f, 2.2f);
            float b = powf(235.0f/255.0f, 2.2f);

            float oc[3] = { ro[0], ro[1]-1.0f, ro[2]-4.0f };
            float b_dot = rd[0]*oc[0] + rd[1]*oc[1] + rd[2]*oc[2];
            float c_val = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - 2.25f;
            float disc = b_dot*b_dot - c_val;
            if (disc > 0.0f) {
                float t = -b_dot - sqrtf(disc);
                if (t > zNear && t < zFar) {
                    hitZ = t;
                    float nx = ro[0]+rd[0]*t;
                    float ny = ro[1]+rd[1]*t - 1.0f;
                    float nz = ro[2]+rd[2]*t - 4.0f;
                    float nl = sqrtf(nx*nx+ny*ny+nz*nz);
                    nx/=nl; ny/=nl; nz/=nl;
                    float light[3] = { 0.577f, 0.577f, -0.577f };
                    float ndotl = fmaxf(0.2f,
                        -(nx*light[0]+ny*light[1]+nz*light[2]));
                    r = powf(200.0f/255.0f, 2.2f) * ndotl;
                    g = powf(50.0f/255.0f,  2.2f) * ndotl;
                    b = powf(50.0f/255.0f,  2.2f) * ndotl;
                }
            }
            if (rd[1] < 0.0f) {
                float t = -ro[1] / rd[1];
                if (t > zNear && t < zFar && (hitZ < 0.0f || t < hitZ)) {
                    hitZ = t;
                    float px = ro[0]+rd[0]*t;
                    float pz = ro[2]+rd[2]*t;
                    int chk = ((int)floorf(px)+(int)floorf(pz)) % 2;
                    float cv = (chk==0) ?
                        powf(220.0f/255.0f,2.2f) : powf(80.0f/255.0f,2.2f);
                    r = g = b = cv;
                }
            }

            int idx = y*w+x;
            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 1.0f;

            depthOut[idx] = (hitZ > 0.0f) ? (zNear / hitZ) : 0.0f;

            mvOut[idx*2+0] = prevJX - currJX;
            mvOut[idx*2+1] = prevJY - currJY;
        }
    }
}

void saveFloatImage(const std::string& filename, int w, int h,
    const float* data) {
    std::vector<unsigned char> bytes(w * h * 4);
    int nanCount = 0;
    for (int i = 0; i < w*h*4; i+=4) {
        for (int c = 0; c < 3; c++) {
            float v = data[i+c];
            if (std::isnan(v)) { v = 0.0f; nanCount++; }
            v = fmaxf(0.0f, fminf(1.0f, v));
            bytes[i+c] = (unsigned char)(powf(v, 1.0f/2.2f) * 255.0f);
        }
        bytes[i+3] = 255;
    }
    if (nanCount > 0)
        std::cout << "[WARNING] " << nanCount << " NaNs in "
                  << filename << std::endl;
    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w*4);
    std::cout << "Saved: " << filename << std::endl;
    std::cout.flush();
}

void saveDepthImage(const std::string& filename, int w, int h,
    const float* data) {
    std::vector<unsigned char> bytes(w * h * 4);
    float minV = 1e30f, maxV = -1e30f;
    for (int i = 0; i < w*h; i++) {
        float v = data[i];
        if (!std::isnan(v) && !std::isinf(v)) {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }
    std::cout << "[Depth] min=" << minV << " max=" << maxV << std::endl;
    float range = (maxV > minV) ? (maxV - minV) : 1.0f;
    for (int i = 0; i < w*h; i++) {
        float v = data[i];
        if (std::isnan(v)) v = 0.0f;
        v = (v - minV) / range;
        v = fmaxf(0.0f, fminf(1.0f, v));
        unsigned char bv = (unsigned char)(v * 255.0f);
        bytes[i*4+0] = bv;
        bytes[i*4+1] = bv;
        bytes[i*4+2] = bv;
        bytes[i*4+3] = 255;
    }
    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w*4);
    std::cout << "Saved: " << filename << std::endl;
    std::cout.flush();
}

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "  FSR Vulkan Testbed                       " << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout.flush();

    ffxAssertSetPrintingCallback(AssertCallback);

    const uint32_t renderW  = 320, renderH  = 240;
    const uint32_t displayW = 640, displayH = 480;

    // =========================================================================
    // VULKAN INIT
    // =========================================================================
    std::cout << "\n[Init] Creating Vulkan instance..." << std::endl;

    if (volkInitialize() != VK_SUCCESS) {
        std::cout << "[FATAL] volk init failed\n"; return 1;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &appInfo;
    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateInstance failed\n"; return 1;
    }
    volkLoadInstance(instance);
    std::cout << "[Init] Vulkan instance created OK." << std::endl;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = physicalDevices[0];

    VkPhysicalDeviceProperties devProps;
    vkGetPhysicalDeviceProperties(physicalDevice, &devProps);
    std::cout << "[Init] Using GPU: " << devProps.deviceName << std::endl;

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCI = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCI.queueFamilyIndex = 0;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features features12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
    VkPhysicalDeviceFeatures2 features2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &extCount, availableExts.data());
    std::vector<const char*> enabledExts;
    for (auto& e : availableExts) enabledExts.push_back(e.extensionName);

    VkDeviceCreateInfo deviceCI = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCI.queueCreateInfoCount = 1;
    deviceCI.pQueueCreateInfos = &queueCI;
    deviceCI.pNext = &features2;
    deviceCI.enabledExtensionCount = (uint32_t)enabledExts.size();
    deviceCI.ppEnabledExtensionNames = enabledExts.data();

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceCI, nullptr, &device)
        != VK_SUCCESS) {
        std::cout << "[FATAL] vkCreateDevice failed\n"; return 1;
    }
    volkLoadDevice(device);
    std::cout << "[Init] Logical device created OK." << std::endl;

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkCommandPoolCreateInfo poolCI = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.queueFamilyIndex = 0;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool;
    vkCreateCommandPool(device, &poolCI, nullptr, &commandPool);

    VkCommandBufferAllocateInfo cmdAllocInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // =========================================================================
    // CPU REFERENCE RENDERS
    // =========================================================================
    std::cout << "\n[CPU] Rendering Native_1x.png (320x240)..." << std::endl;
    {
        std::vector<float> native1x(renderW * renderH * 4);
        std::vector<float> depth1x(renderW * renderH);
        std::vector<float> mv1x(renderW * renderH * 2, 0.0f);
        renderScene(renderW, renderH, 0.0f, 0.0f, 0.0f, 0.0f,
            native1x.data(), depth1x.data(), mv1x.data());
        saveFloatImage("Native_1x.png", renderW, renderH, native1x.data());
        saveDepthImage("Native_1x_depth.png", renderW, renderH, depth1x.data());
    }

    std::cout << "[CPU] Rendering Native_2x.png (640x480)..." << std::endl;
    {
        std::vector<float> native2x(displayW * displayH * 4);
        std::vector<float> depth2x(displayW * displayH);
        std::vector<float> mv2x(displayW * displayH * 2, 0.0f);
        renderScene(displayW, displayH, 0.0f, 0.0f, 0.0f, 0.0f,
            native2x.data(), depth2x.data(), mv2x.data());
        saveFloatImage("Native_2x.png", displayW, displayH, native2x.data());
    }

    // =========================================================================
    // SHARED VULKAN RESOURCES
    // =========================================================================
    VkDeviceSize colorUploadSize = renderW * renderH * 4 * sizeof(float);
    VkDeviceSize depthUploadSize = renderW * renderH * 1 * sizeof(float);
    VkDeviceSize mvUploadSize    = renderW * renderH * 2 * sizeof(float);
    VkDeviceSize uploadSize      = colorUploadSize + depthUploadSize
                                   + mvUploadSize;

    VkBuffer uploadBuffer; VkDeviceMemory uploadMemory;
    createBuffer(device, physicalDevice, uploadSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        uploadBuffer, uploadMemory);

    VkDeviceSize outSize = displayW * displayH * 4 * sizeof(float);
    VkBuffer downloadBuffer; VkDeviceMemory downloadMemory;
    createBuffer(device, physicalDevice, outSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        downloadBuffer, downloadMemory);

    VkImage colorImage;  VkDeviceMemory colorMem;
    VkImage depthImage;  VkDeviceMemory depthMem;
    VkImage mvImage;     VkDeviceMemory mvMem;
    VkImage outputImage; VkDeviceMemory outputMem;
    VkImage expImage;    VkDeviceMemory expMem;

    VkImageCreateInfo colorInfo = createImage(device, physicalDevice,
        renderW, renderH, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        colorImage, colorMem);

    VkImageCreateInfo depthInfo = createImage(device, physicalDevice,
        renderW, renderH, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        depthImage, depthMem);

    VkImageCreateInfo mvInfo = createImage(device, physicalDevice,
        renderW, renderH, VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        mvImage, mvMem);

    VkImageCreateInfo outputInfo = createImage(device, physicalDevice,
        displayW, displayH, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        outputImage, outputMem);

    VkImageCreateInfo expInfo = createImage(device, physicalDevice,
        1, 1, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        expImage, expMem);

    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout expLayout    = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageSubresourceRange colorRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDeviceContext vkDeviceContext = {
        device, physicalDevice, vkGetDeviceProcAddr };

    // We need separate scratch buffers for FSR1, FSR2, and FSR3 because
    // each effect context uses its own scratch allocation internally.
    // Using a single buffer shared between multiple contexts would
    // cause memory corruption as each call to ffxGetInterfaceVK
    // sets up its own internal allocator within the same memory region.
    size_t safeBufferSize =
        ffxGetScratchMemorySizeVK(physicalDevice, 4) * 2;

    void* scratchBuffer1 = _aligned_malloc(safeBufferSize, 64);
    void* scratchBuffer2 = _aligned_malloc(safeBufferSize, 64);
    void* scratchBuffer3 = _aligned_malloc(safeBufferSize, 64);

    VkBufferImageCopy outRegion = {};
    outRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    outRegion.imageExtent = { displayW, displayH, 1 };

    // =========================================================================
    // FSR 1.2 - Spatial Upscaling
    // =========================================================================
    std::cout << "\n============================================" << std::endl;
    std::cout << "  FSR 1.2 Spatial Upscaling Test" << std::endl;
    std::cout << "============================================" << std::endl;
    memset(scratchBuffer1, 0, safeBufferSize);

    FfxInterface ffxInterface1 = {};
    ffxGetInterfaceVK(&ffxInterface1, ffxGetDeviceVK(&vkDeviceContext),
        scratchBuffer1, safeBufferSize, 4);

    FfxFsr1ContextDescription fsr1Desc = {};
    fsr1Desc.flags        = FFX_FSR1_ENABLE_HIGH_DYNAMIC_RANGE |
                            FFX_FSR1_ENABLE_RCAS;
    fsr1Desc.outputFormat     = ffxGetSurfaceFormatVK(VK_FORMAT_R32G32B32A32_SFLOAT);
    fsr1Desc.maxRenderSize    = { renderW, renderH };
    fsr1Desc.displaySize      = { displayW, displayH };
    fsr1Desc.backendInterface = ffxInterface1;

    FfxFsr1Context* fsr1Context =
        (FfxFsr1Context*)_aligned_malloc(sizeof(FfxFsr1Context), 64);
    memset(fsr1Context, 0, sizeof(FfxFsr1Context));
    if (ffxFsr1ContextCreate(fsr1Context, &fsr1Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr1ContextCreate failed!\n"; return 1;
    }

    {
        std::vector<float> fsr1Color(renderW * renderH * 4);
        std::vector<float> fsr1Depth(renderW * renderH);
        std::vector<float> fsr1MV(renderW * renderH * 2, 0.0f);
        renderScene(renderW, renderH, 0.0f, 0.0f, 0.0f, 0.0f,
            fsr1Color.data(), fsr1Depth.data(), fsr1MV.data());
        void* mappedData;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
        memcpy(mappedData, fsr1Color.data(), colorUploadSize);
        vkUnmapMemory(device, uploadMemory);
    }

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transition(cmd, colorImage, colorLayout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy cRegion = {};
    cRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cRegion.imageExtent = { renderW, renderH, 1 };
    vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion);

    transition(cmd, colorImage, colorLayout,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    FfxResource colorRes1 = ffxGetResourceVK(colorImage,
        ffxGetImageResourceDescriptionVK(colorImage, colorInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR1_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes1 = ffxGetResourceVK(outputImage,
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo,
            FFX_RESOURCE_USAGE_UAV),
        L"FSR1_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxFsr1DispatchDescription disp1 = {};
    disp1.commandList      = ffxGetCommandListVK(cmd);
    disp1.color            = colorRes1;
    disp1.output           = outputRes1;
    disp1.renderSize       = { renderW, renderH };
    disp1.enableSharpening = true;
    disp1.sharpness        = 0.8f;

    std::cout << "[FSR1] Dispatching FSR 1.2 upscaler..." << std::endl;
    if (ffxFsr1ContextDispatch(fsr1Context, &disp1) != FFX_OK) {
        std::cout << "[FATAL] FSR1 dispatch failed!\n"; return 1;
    }

    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImageToBuffer(cmd, outputImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);

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

    // =========================================================================
    // FSR 2.3.3 - Temporal Upscaling
    // =========================================================================
    std::cout << "\n============================================" << std::endl;
    std::cout << "  FSR 2.3.3 Temporal Upscaling Test (128 frames)" << std::endl;
    std::cout << "============================================" << std::endl;
    memset(scratchBuffer2, 0, safeBufferSize);

    FfxInterface ffxInterface2 = {};
    ffxGetInterfaceVK(&ffxInterface2, ffxGetDeviceVK(&vkDeviceContext),
        scratchBuffer2, safeBufferSize, 4);

    FfxFsr2ContextDescription fsr2Desc = {};
    memset(&fsr2Desc, 0, sizeof(fsr2Desc));
    fsr2Desc.flags =
        FFX_FSR2_ENABLE_DEBUG_CHECKING     |
        FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_FSR2_ENABLE_AUTO_EXPOSURE      |
        FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
        FFX_FSR2_ENABLE_DEPTH_INVERTED;

    fsr2Desc.maxRenderSize    = { renderW, renderH };
    fsr2Desc.displaySize      = { displayW, displayH };
    fsr2Desc.fpMessage        = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface2;

    FfxFsr2Context* fsr2Context =
        (FfxFsr2Context*)_aligned_malloc(sizeof(FfxFsr2Context), 64);
    memset(fsr2Context, 0, sizeof(FfxFsr2Context));
    if (ffxFsr2ContextCreate(fsr2Context, &fsr2Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr2ContextCreate failed!\n"; return 1;
    }
    std::cout << "[FSR2] Context created OK." << std::endl;

    {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);
        outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, outputImage, outputLayout,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mvLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    expLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    FfxResource colorRes2 = ffxGetResourceVK(colorImage,
        ffxGetImageResourceDescriptionVK(colorImage, colorInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes2 = ffxGetResourceVK(depthImage,
        ffxGetImageResourceDescriptionVK(depthImage, depthInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes2 = ffxGetResourceVK(mvImage,
        ffxGetImageResourceDescriptionVK(mvImage, mvInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR2_MVs", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes2 = ffxGetResourceVK(outputImage,
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo,
            FFX_RESOURCE_USAGE_UAV),
        L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    int32_t phaseCount = ffxFsr2GetJitterPhaseCount(renderW, displayW);
    std::cout << "[FSR2] Jitter phase count: " << phaseCount << std::endl;

    const int totalFrames = 128;
    std::cout << "[FSR2] Running " << totalFrames
              << " temporal accumulation frames..." << std::endl;
    std::cout.flush();

    int32_t jitterIndex = 0;
    float prevJX = 0.0f, prevJY = 0.0f;

    for (int i = 0; i < totalFrames; i++) {
        ++jitterIndex;

        float jX = 0.0f, jY = 0.0f;
        ffxFsr2GetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

        std::vector<float> fColor(renderW * renderH * 4);
        std::vector<float> fDepth(renderW * renderH);
        std::vector<float> fMV(renderW * renderH * 2);

        renderScene(renderW, renderH, jX, jY, prevJX, prevJY,
            fColor.data(), fDepth.data(), fMV.data());

        void* mappedData;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
        memcpy((uint8_t*)mappedData, fColor.data(), colorUploadSize);
        memcpy((uint8_t*)mappedData + colorUploadSize,
            fDepth.data(), depthUploadSize);
        memcpy((uint8_t*)mappedData + colorUploadSize + depthUploadSize,
            fMV.data(), mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cR2 = {};
        cR2.bufferOffset = 0;
        cR2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cR2.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR2);

        VkBufferImageCopy dR2 = {};
        dR2.bufferOffset = colorUploadSize;
        dR2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        dR2.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR2);

        VkBufferImageCopy mR2 = {};
        mR2.bufferOffset = colorUploadSize + depthUploadSize;
        mR2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        mR2.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, mvImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR2);

        VkClearColorValue expClear = {{ 1.0f, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(cmd, expImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &expClear, 1, &colorRange);

        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        FfxFsr2DispatchDescription dispatchDesc = {};
        memset(&dispatchDesc, 0, sizeof(dispatchDesc));
        dispatchDesc.commandList   = ffxGetCommandListVK(cmd);
        dispatchDesc.color         = colorRes2;
        dispatchDesc.depth         = depthRes2;
        dispatchDesc.motionVectors = mvRes2;
        dispatchDesc.output        = outputRes2;

        dispatchDesc.jitterOffset.x = jX;
        dispatchDesc.jitterOffset.y = jY;

        dispatchDesc.motionVectorScale.x = (float)renderW;
        dispatchDesc.motionVectorScale.y = (float)renderH;

        dispatchDesc.renderSize = { renderW, renderH };

        dispatchDesc.enableSharpening = true;
        dispatchDesc.sharpness        = 0.8f;
        dispatchDesc.frameTimeDelta   = 16.6f;
        dispatchDesc.preExposure      = 1.0f;
        dispatchDesc.reset            = (i == 0);

        dispatchDesc.cameraNear             = FLT_MAX;
        dispatchDesc.cameraFar              = 0.1f;
        dispatchDesc.cameraFovAngleVertical = 1.04719755f;
        dispatchDesc.viewSpaceToMetersFactor = 1.0f;

        if (ffxFsr2ContextDispatch(fsr2Context, &dispatchDesc) != FFX_OK) {
            std::cout << "[FATAL] FSR2 dispatch failed frame "
                      << i << std::endl;
            return 1;
        }

        vkEndCommandBuffer(cmd);

        VkResult submitResult = vkQueueSubmit(
            queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (submitResult != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueSubmit failed frame " << i
                      << " VkResult=" << submitResult << std::endl;
            return 1;
        }
        VkResult waitResult = vkQueueWaitIdle(queue);
        if (waitResult != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueWaitIdle failed frame " << i
                      << " VkResult=" << waitResult << std::endl;
            return 1;
        }

        int frameNum = i + 1;
        if (frameNum == 32 || frameNum == 64 ||
            frameNum == 96 || frameNum == 128) {

            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &beginInfo);
            transition(cmd, outputImage, outputLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdCopyImageToBuffer(cmd, outputImage,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                downloadBuffer, 1, &outRegion);
            transition(cmd, outputImage, outputLayout,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
            std::string fname = "FSR_2.3.3_frame" +
                                std::to_string(frameNum) + ".png";
            saveFloatImage(fname, displayW, displayH, (float*)outData);
            vkUnmapMemory(device, downloadMemory);

            std::cout << "[FSR2] Snapshot saved at frame "
                      << frameNum << std::endl;
        }

        if (frameNum % 32 == 0)
            std::cout << "[FSR2] Frame " << frameNum << "/"
                      << totalFrames << " done" << std::endl;

        prevJX = jX;
        prevJY = jY;
    }

    // Final download
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);
    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImageToBuffer(cmd, outputImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_2.3.3_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr2ContextDestroy(fsr2Context);
    _aligned_free(fsr2Context);
    std::cout << "[FSR2] Done." << std::endl;

    // =========================================================================
    // FSR 3.1.4 - Temporal Upscaling (upscaler only, no frame generation)
    // =========================================================================
    std::cout << "\n============================================" << std::endl;
    std::cout << "  FSR 3.1.4 Temporal Upscaling Test (128 frames)" << std::endl;
    std::cout << "============================================" << std::endl;
    memset(scratchBuffer3, 0, safeBufferSize);

    FfxInterface ffxInterface3 = {};
    ffxGetInterfaceVK(&ffxInterface3, ffxGetDeviceVK(&vkDeviceContext),
        scratchBuffer3, safeBufferSize, 4);

    // Create FSR3 upscaler context description.
    // We use FFX_FSR3UPSCALER_ENABLE_UPSCALING_ONLY via the upscaler-specific
    // flags. Frame generation and optical flow are not used in this testbed.
    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    memset(&fsr3Desc, 0, sizeof(fsr3Desc));
    fsr3Desc.flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE      |
        FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
        FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED     |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    fsr3Desc.maxRenderSize  = { renderW, renderH };
    fsr3Desc.maxUpscaleSize = { displayW, displayH };
    fsr3Desc.fpMessage      = Fsr3MessageCallback;
    fsr3Desc.backendInterface = ffxInterface3;

    FfxFsr3UpscalerContext* fsr3Context =
        (FfxFsr3UpscalerContext*)_aligned_malloc(
            sizeof(FfxFsr3UpscalerContext), 64);
    memset(fsr3Context, 0, sizeof(FfxFsr3UpscalerContext));

    if (ffxFsr3UpscalerContextCreate(fsr3Context, &fsr3Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr3UpscalerContextCreate failed!\n";
        return 1;
    }
    std::cout << "[FSR3] Context created OK." << std::endl;

    // Query shared resource descriptions.
    // FSR3 requires three externally-allocated shared resources:
    //   - reconstructedPrevNearestDepth
    //   - dilatedDepth
    //   - dilatedMotionVectors
    // These are written by the upscaler and can be read by frame generation
    // (which we don't use here, but we must still allocate them).
    FfxFsr3UpscalerSharedResourceDescriptions sharedResDescs = {};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(
            fsr3Context, &sharedResDescs) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr3UpscalerGetSharedResourceDescriptions failed!\n";
        return 1;
    }

    std::cout << "[FSR3] Shared resource descriptions:" << std::endl;
    std::cout << "[FSR3]   reconstructedPrevNearestDepth: "
              << sharedResDescs.reconstructedPrevNearestDepth.resourceDescription.width
              << "x"
              << sharedResDescs.reconstructedPrevNearestDepth.resourceDescription.height
              << " fmt=" << (int)sharedResDescs.reconstructedPrevNearestDepth.resourceDescription.format
              << std::endl;
    std::cout << "[FSR3]   dilatedDepth: "
              << sharedResDescs.dilatedDepth.resourceDescription.width
              << "x"
              << sharedResDescs.dilatedDepth.resourceDescription.height
              << " fmt=" << (int)sharedResDescs.dilatedDepth.resourceDescription.format
              << std::endl;
    std::cout << "[FSR3]   dilatedMotionVectors: "
              << sharedResDescs.dilatedMotionVectors.resourceDescription.width
              << "x"
              << sharedResDescs.dilatedMotionVectors.resourceDescription.height
              << " fmt=" << (int)sharedResDescs.dilatedMotionVectors.resourceDescription.format
              << std::endl;
    std::cout.flush();

    // Allocate the three shared images from the descriptions returned by the SDK.
    SharedImage siReconstructed = createSharedImage(device, physicalDevice,
        sharedResDescs.reconstructedPrevNearestDepth);
    SharedImage siDilatedDepth  = createSharedImage(device, physicalDevice,
        sharedResDescs.dilatedDepth);
    SharedImage siDilatedMV     = createSharedImage(device, physicalDevice,
        sharedResDescs.dilatedMotionVectors);

    std::cout << "[FSR3] Shared images allocated OK." << std::endl;

    // Transition all shared images to GENERAL so FSR3 can read/write freely.
    {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transition(cmd, siReconstructed.image, siReconstructed.layout,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
            siReconstructed.info.mipLevels);
        transition(cmd, siDilatedDepth.image, siDilatedDepth.layout,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
            siDilatedDepth.info.mipLevels);
        transition(cmd, siDilatedMV.image, siDilatedMV.layout,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
            siDilatedMV.info.mipLevels);

        // Also reset output image layout for FSR3.
        outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, outputImage, outputLayout,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    // Reset input image layouts for FSR3 frame loop.
    colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mvLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    expLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    // Build FfxResource wrappers for the fixed images.
    // The shared resources need UAV usage since FSR3 writes into them.
    FfxResource colorRes3 = ffxGetResourceVK(colorImage,
        ffxGetImageResourceDescriptionVK(colorImage, colorInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR3_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes3 = ffxGetResourceVK(depthImage,
        ffxGetImageResourceDescriptionVK(depthImage, depthInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR3_Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes3 = ffxGetResourceVK(mvImage,
        ffxGetImageResourceDescriptionVK(mvImage, mvInfo,
            FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR3_MVs", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes3 = ffxGetResourceVK(outputImage,
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo,
            FFX_RESOURCE_USAGE_UAV),
        L"FSR3_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    // Build FfxResource wrappers for shared resources.
    // We pass the VkImageCreateInfo from createSharedImage so the description
    // is accurate for the actual allocated image.
    FfxResource reconRes = ffxGetResourceVK(siReconstructed.image,
        ffxGetImageResourceDescriptionVK(siReconstructed.image,
            siReconstructed.info, FFX_RESOURCE_USAGE_UAV),
        L"FSR3_ReconPrevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    FfxResource dilDepthRes = ffxGetResourceVK(siDilatedDepth.image,
        ffxGetImageResourceDescriptionVK(siDilatedDepth.image,
            siDilatedDepth.info, FFX_RESOURCE_USAGE_UAV),
        L"FSR3_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    FfxResource dilMVRes = ffxGetResourceVK(siDilatedMV.image,
        ffxGetImageResourceDescriptionVK(siDilatedMV.image,
            siDilatedMV.info, FFX_RESOURCE_USAGE_UAV),
        L"FSR3_DilatedMV", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    int32_t phaseCount3 =
        ffxFsr3UpscalerGetJitterPhaseCount(renderW, displayW);
    std::cout << "[FSR3] Jitter phase count: " << phaseCount3 << std::endl;

    const int totalFrames3 = 128;
    std::cout << "[FSR3] Running " << totalFrames3
              << " temporal accumulation frames..." << std::endl;
    std::cout.flush();

    int32_t jitterIndex3 = 0;
    float prevJX3 = 0.0f, prevJY3 = 0.0f;

    for (int i = 0; i < totalFrames3; i++) {
        ++jitterIndex3;

        float jX3 = 0.0f, jY3 = 0.0f;
        ffxFsr3UpscalerGetJitterOffset(&jX3, &jY3, jitterIndex3, phaseCount3);

        std::vector<float> fColor3(renderW * renderH * 4);
        std::vector<float> fDepth3(renderW * renderH);
        std::vector<float> fMV3(renderW * renderH * 2);

        renderScene(renderW, renderH, jX3, jY3, prevJX3, prevJY3,
            fColor3.data(), fDepth3.data(), fMV3.data());

        void* mappedData3;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData3);
        memcpy((uint8_t*)mappedData3, fColor3.data(), colorUploadSize);
        memcpy((uint8_t*)mappedData3 + colorUploadSize,
            fDepth3.data(), depthUploadSize);
        memcpy((uint8_t*)mappedData3 + colorUploadSize + depthUploadSize,
            fMV3.data(), mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Upload inputs.
        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cR3 = {};
        cR3.bufferOffset = 0;
        cR3.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cR3.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR3);

        VkBufferImageCopy dR3 = {};
        dR3.bufferOffset = colorUploadSize;
        dR3.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        dR3.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR3);

        VkBufferImageCopy mR3 = {};
        mR3.bufferOffset = colorUploadSize + depthUploadSize;
        mR3.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        mR3.imageExtent = { renderW, renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, mvImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR3);

        transition(cmd, colorImage, colorLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // Fill in FSR3 upscaler dispatch description.
        // Key differences from FSR2:
        //   - dilatedDepth, dilatedMotionVectors, reconstructedPrevNearestDepth
        //     are explicit external resources the caller manages.
        //   - upscaleSize replaces displaySize (FSR3 supports asymmetric output).
        //   - No separate exposure resource needed when AUTO_EXPOSURE is set.
        //   - frameID is not present in FfxFsr3UpscalerDispatchDescription
        //     (it is only in FfxFsr3DispatchUpscaleDescription used by the
        //     full FSR3 context which includes frame generation).
        FfxFsr3UpscalerDispatchDescription dispatchDesc3 = {};
        memset(&dispatchDesc3, 0, sizeof(dispatchDesc3));

        dispatchDesc3.commandList    = ffxGetCommandListVK(cmd);
        dispatchDesc3.color          = colorRes3;
        dispatchDesc3.depth          = depthRes3;
        dispatchDesc3.motionVectors  = mvRes3;
        dispatchDesc3.output         = outputRes3;

        // Shared resources (written by this pass, potentially read by FG).
        dispatchDesc3.dilatedDepth               = dilDepthRes;
        dispatchDesc3.dilatedMotionVectors        = dilMVRes;
        dispatchDesc3.reconstructedPrevNearestDepth = reconRes;

        // Optional resources - pass null (zero-initialised FfxResource).
        // exposure:                 not provided, AUTO_EXPOSURE flag is set.
        // reactive:                 not provided (no reactive mask).
        // transparencyAndComposition: not provided.
        // These are already zero from memset above.

        dispatchDesc3.jitterOffset.x = jX3;
        dispatchDesc3.jitterOffset.y = jY3;

        dispatchDesc3.motionVectorScale.x = (float)renderW;
        dispatchDesc3.motionVectorScale.y = (float)renderH;

        dispatchDesc3.renderSize  = { renderW,  renderH  };
        dispatchDesc3.upscaleSize = { displayW, displayH };

        dispatchDesc3.enableSharpening = true;
        dispatchDesc3.sharpness        = 0.8f;
        dispatchDesc3.frameTimeDelta   = 16.6f;
        dispatchDesc3.preExposure      = 1.0f;
        dispatchDesc3.reset            = (i == 0);

        // Inverted depth: near=FLT_MAX, far=actual near plane value.
        dispatchDesc3.cameraNear             = FLT_MAX;
        dispatchDesc3.cameraFar              = 0.1f;
        dispatchDesc3.cameraFovAngleVertical = 1.04719755f;
        dispatchDesc3.viewSpaceToMetersFactor = 1.0f;

        dispatchDesc3.flags = 0; // no debug view

        if (ffxFsr3UpscalerContextDispatch(fsr3Context, &dispatchDesc3)
                != FFX_OK) {
            std::cout << "[FATAL] FSR3 dispatch failed frame "
                      << i << std::endl;
            return 1;
        }

        vkEndCommandBuffer(cmd);

        VkResult submitResult3 = vkQueueSubmit(
            queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (submitResult3 != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueSubmit failed frame " << i
                      << " VkResult=" << submitResult3 << std::endl;
            return 1;
        }
        VkResult waitResult3 = vkQueueWaitIdle(queue);
        if (waitResult3 != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueWaitIdle failed frame " << i
                      << " VkResult=" << waitResult3 << std::endl;
            return 1;
        }

        int frameNum3 = i + 1;
        if (frameNum3 == 32 || frameNum3 == 64 ||
            frameNum3 == 96 || frameNum3 == 128) {

            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &beginInfo);
            transition(cmd, outputImage, outputLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdCopyImageToBuffer(cmd, outputImage,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                downloadBuffer, 1, &outRegion);
            transition(cmd, outputImage, outputLayout,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
            std::string fname3 = "FSR_3.1.4_frame" +
                                 std::to_string(frameNum3) + ".png";
            saveFloatImage(fname3, displayW, displayH, (float*)outData);
            vkUnmapMemory(device, downloadMemory);

            std::cout << "[FSR3] Snapshot saved at frame "
                      << frameNum3 << std::endl;
        }

        if (frameNum3 % 32 == 0)
            std::cout << "[FSR3] Frame " << frameNum3 << "/"
                      << totalFrames3 << " done" << std::endl;

        prevJX3 = jX3;
        prevJY3 = jY3;
    }

    // Final FSR3 download.
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);
    transition(cmd, outputImage, outputLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImageToBuffer(cmd, outputImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_3.1.4_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr3UpscalerContextDestroy(fsr3Context);
    _aligned_free(fsr3Context);
    std::cout << "[FSR3] Done." << std::endl;

    // =========================================================================
    // CLEANUP
    // =========================================================================
    vkDestroyImage(device, siReconstructed.image, nullptr);
    vkFreeMemory(device,   siReconstructed.memory, nullptr);
    vkDestroyImage(device, siDilatedDepth.image, nullptr);
    vkFreeMemory(device,   siDilatedDepth.memory, nullptr);
    vkDestroyImage(device, siDilatedMV.image, nullptr);
    vkFreeMemory(device,   siDilatedMV.memory, nullptr);

    vkDestroyImage(device, colorImage,   nullptr);
    vkFreeMemory(device,   colorMem,     nullptr);
    vkDestroyImage(device, depthImage,   nullptr);
    vkFreeMemory(device,   depthMem,     nullptr);
    vkDestroyImage(device, mvImage,      nullptr);
    vkFreeMemory(device,   mvMem,        nullptr);
    vkDestroyImage(device, outputImage,  nullptr);
    vkFreeMemory(device,   outputMem,    nullptr);
    vkDestroyImage(device, expImage,     nullptr);
    vkFreeMemory(device,   expMem,       nullptr);
    vkDestroyBuffer(device, uploadBuffer,    nullptr);
    vkFreeMemory(device,    uploadMemory,    nullptr);
    vkDestroyBuffer(device, downloadBuffer,  nullptr);
    vkFreeMemory(device,    downloadMemory,  nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    _aligned_free(scratchBuffer1);
    _aligned_free(scratchBuffer2);
    _aligned_free(scratchBuffer3);

    std::cout << "\n[Done] All tests complete." << std::endl;
    return 0;
}

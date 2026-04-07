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

// =========================================================================
// AMD SDK C++ INTERCEPTORS (Guaranteed to bypass SwiftShader Emulator Bugs!)
// =========================================================================
static FfxGetDeviceCapabilitiesFunc g_original_fpGetDeviceCapabilities = nullptr;
static FfxCreateResourceFunc g_original_fpCreateResource = nullptr;

FfxErrorCode CustomGetDeviceCapabilities(FfxInterface* backendInterface, FfxDeviceCapabilities* outDeviceCapabilities, FfxDevice device) {
    FfxErrorCode code = g_original_fpGetDeviceCapabilities(backendInterface, outDeviceCapabilities, device);
    if (code == FFX_OK) {
        outDeviceCapabilities->fp16Supported = false; // KILL-SWITCH FOR SWIFTSHADER FP16 BUG
        std::cout << "[Trace-VK] Intercepted fpGetDeviceCapabilities! Forced fp16Supported = false\n";
        std::cout.flush();
    }
    return code;
}

// SDK v1.1.4 requires the 5-argument signature for unified multi-effect contexts
FfxErrorCode CustomCreateResource(
    FfxInterface* backendInterface, 
    const FfxCreateResourceDescription* desc, 
    FfxEffect effect, 
    FfxUInt32* effectContextId, 
    FfxResourceInternal* outTexture) 
{
    FfxCreateResourceDescription modifiedDesc = *desc;
    // KILL-SWITCH FOR SWIFTSHADER R11G11B10 UAV DROP BUG
    if (modifiedDesc.resourceDescription.format == FFX_SURFACE_FORMAT_R11G11B10_FLOAT) {
        modifiedDesc.resourceDescription.format = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        std::cout << "[Trace-VK] Intercepted fpCreateResource! Upgraded R11G11B10 to R16G16B16A16\n";
        std::cout.flush();
    }
    return g_original_fpCreateResource(backendInterface, &modifiedDesc, effect, effectContextId, outTexture);
}
// =========================================================================

static void AssertCallback(const char* message) {
    std::cout << "\n[AMD SDK ASSERTION FAILED] " << message << std::endl;
}

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        std::cout << "[AMD SDK] ";
        for (int i = 0; i < 2048 && message[i] != 0; ++i) {
            std::cout << (char)message[i];
        }
        std::cout << std::endl;
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

// --- PROCEDURAL 3D RAYTRACER (HDR LINEAR FLOATS + INVERTED DEPTH) ---
void renderScene(int w, int h, float jx, float jy, float* colorOut, float* depthOut) {
    float aspect = (float)w / (float)h;
    float fovY = 1.04719755f;
    float tanHalfFov = tanf(fovY / 2.0f);
    float zNear = 0.1f;
    float zFar = 100.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float ndcX = ((x + 0.5f + jx) / w) * 2.0f - 1.0f;
            float ndcY = ((y + 0.5f + jy) / h) * 2.0f - 1.0f;

            float ro[3] = {0.0f, 1.0f, 0.0f};
            float rd[3] = {ndcX * aspect * tanHalfFov, -ndcY * tanHalfFov, 1.0f};
            float len = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0]/=len; rd[1]/=len; rd[2]/=len;

            float hitZ = -1.0f;
            float r = powf(135.0f/255.0f, 2.2f), g = powf(206.0f/255.0f, 2.2f), b = powf(235.0f/255.0f, 2.2f);

            float oc[3] = {ro[0] - 0.0f, ro[1] - 1.0f, ro[2] - 4.0f};
            float b_dot = rd[0]*oc[0] + rd[1]*oc[1] + rd[2]*oc[2];
            float c_val = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - (1.5f * 1.5f);
            float disc = b_dot*b_dot - c_val;

            if (disc > 0.0f) {
                float t = -b_dot - sqrtf(disc);
                if (t > zNear && t < zFar) {
                    hitZ = t;
                    float nx = (ro[0] + rd[0]*t) - 0.0f;
                    float ny = (ro[1] + rd[1]*t) - 1.0f;
                    float nz = (ro[2] + rd[2]*t) - 4.0f;
                    float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
                    nx/=nlen; ny/=nlen; nz/=nlen;

                    float light[3] = {0.577f, 0.577f, -0.577f};
                    float ndotl = fmax(0.2f, -(nx*light[0] + ny*light[1] + nz*light[2]));
                    r = powf(200.0f/255.0f, 2.2f) * ndotl;
                    g = powf(50.0f/255.0f, 2.2f) * ndotl;
                    b = powf(50.0f/255.0f, 2.2f) * ndotl;
                }
            }

            if (rd[1] < 0.0f) {
                float t = -ro[1] / rd[1];
                if (t > zNear && t < zFar && (hitZ < 0.0f || t < hitZ)) {
                    hitZ = t;
                    float px = ro[0] + rd[0]*t;
                    float pz = ro[2] + rd[2]*t;
                    int chk = ((int)floorf(px) + (int)floorf(pz)) % 2;
                    if (chk == 0) { r = powf(220.0f/255.0f, 2.2f); g = r; b = r; }
                    else          { r = powf(80.0f/255.0f, 2.2f);  g = r; b = r; }
                }
            }

            int idx = y * w + x;
            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 1.0f;

            if (hitZ > 0.0f) {
                float depth = (zNear * (zFar - hitZ)) / (hitZ * (zFar - zNear));
                depthOut[idx] = depth;
            } else {
                depthOut[idx] = 0.0f; 
            }
        }
    }
}

void saveFloatImage(const std::string& filename, int w, int h, const float* data) {
    std::vector<unsigned char> bytes(w * h * 4);
    int nanCount = 0;
    
    for (int i = 0; i < w * h * 4; i+=4) {
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
        std::cout << "\n[WARNING] " << nanCount << " NaNs (Corrupted Pixels) detected in " << filename << "!\n";
    }

    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w * 4);
    std::cout << "Saved " << filename << " successfully!" << std::endl;
}

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

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout& currentLayout, VkImageLayout newLayout, VkImageAspectFlags aspectMask) {
    if (currentLayout == newLayout) return;

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = currentLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    currentLayout = newLayout;
}

int main() {
    std::cout << "--- FSR 3D Ablation Study (128-Bit Float Pipeline) ---" << std::endl;
    std::cout.flush();

    ffxAssertSetPrintingCallback(AssertCallback);

    uint32_t renderW = 320, renderH = 240;
    uint32_t displayW = 640, displayH = 480;

    if (volkInitialize() != VK_SUCCESS) return 1;

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3; 
    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &appInfo;
    VkInstance instance;
    vkCreateInstance(&instanceInfo, nullptr, &instance);
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

    int queueFamilyIndex = 0;
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &features12 };
    VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11 };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, availableExts.data());
    std::vector<const char*> enabledExtensions;
    for (const auto& ext : availableExts) enabledExtensions.push_back(ext.extensionName);

    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceInfo.pNext = &features2; 
    deviceInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

    VkDevice device;
    vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
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

    std::cout << "Rendering Native_1x.png (320x240)..." << std::endl;
    std::vector<float> native1x(renderW * renderH * 4);
    std::vector<float> depth1x(renderW * renderH);
    renderScene(renderW, renderH, 0.0f, 0.0f, native1x.data(), depth1x.data());
    saveFloatImage("Native_1x.png", renderW, renderH, native1x.data());

    std::cout << "Rendering Native_2x.png (640x480)..." << std::endl;
    std::vector<float> native2x(displayW * displayH * 4);
    std::vector<float> depth2x(displayW * displayH);
    renderScene(displayW, displayH, 0.0f, 0.0f, native2x.data(), depth2x.data());
    saveFloatImage("Native_2x.png", displayW, displayH, native2x.data());

    VkDeviceSize uploadSize = (renderW * renderH * 4 * sizeof(float)) + (renderW * renderH * sizeof(float));
    VkBuffer uploadBuffer; VkDeviceMemory uploadMemory;
    createBuffer(device, physicalDevice, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uploadBuffer, uploadMemory);

    VkBuffer downloadBuffer; VkDeviceMemory downloadMemory;
    VkDeviceSize outSize = displayW * displayH * 4 * sizeof(float);
    createBuffer(device, physicalDevice, outSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, downloadBuffer, downloadMemory);

    VkImage colorImage, depthImage, mvImage, outputImage;
    VkDeviceMemory colorMem, depthMem, mvMem, outputMem;

    VkImageCreateInfo colorInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, colorImage, colorMem);
    VkImageCreateInfo depthInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, depthImage, depthMem);
    VkImageCreateInfo mvInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, mvImage, mvMem);
    VkImageCreateInfo outputInfo = createImage(device, physicalDevice, displayW, displayH, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, outputImage, outputMem);

    VkImage expImage; VkDeviceMemory expImageMem;
    VkImageCreateInfo expInfo = createImage(device, physicalDevice, 1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, expImage, expImageMem);

    VkDeviceContext vkDeviceContext = { device, physicalDevice, vkGetDeviceProcAddr };
    std::cout << "\n[Trace] Allocating Backend Scratch Memory..." << std::flush;
    size_t safeBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 4) * 2;
    void* scratchBuffer = _aligned_malloc(safeBufferSize, 64);
    std::cout << " OK!" << std::endl; std::cout.flush();

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout expLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // =========================================================================
    // FSR 1.2 TEST
    // =========================================================================
    std::cout << "\n=== FSR 1.2 TEST ===" << std::endl; std::cout.flush();
    memset(scratchBuffer, 0, safeBufferSize);
    
    FfxInterface ffxInterface1 = {};
    if (ffxGetInterfaceVK(&ffxInterface1, ffxGetDeviceVK(&vkDeviceContext), scratchBuffer, safeBufferSize, 4) != FFX_OK) return 1;

    FfxFsr1ContextDescription fsr1Desc = {};
    fsr1Desc.flags = FFX_FSR1_ENABLE_RCAS | FFX_FSR1_ENABLE_HIGH_DYNAMIC_RANGE; 
    fsr1Desc.outputFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R32G32B32A32_SFLOAT);
    fsr1Desc.maxRenderSize = { (uint32_t)renderW, (uint32_t)renderH };
    fsr1Desc.displaySize = { displayW, displayH };
    fsr1Desc.backendInterface = ffxInterface1;

    FfxFsr1Context* fsr1Context = (FfxFsr1Context*)_aligned_malloc(sizeof(FfxFsr1Context), 64);
    memset(fsr1Context, 0, sizeof(FfxFsr1Context));
    if (ffxFsr1ContextCreate(fsr1Context, &fsr1Desc) != FFX_OK) return 1;

    FfxResource colorRes1 = ffxGetResourceVK(colorImage, ffxGetImageResourceDescriptionVK(colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes1 = ffxGetResourceVK(outputImage, ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV), L"Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    std::cout << "Executing FSR 1.2 Spatial Upscaler..." << std::endl; std::cout.flush();

    void* mappedData;
    vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
    memcpy(mappedData, native1x.data(), native1x.size() * sizeof(float));
    vkUnmapMemory(device, uploadMemory);

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transition(cmd, colorImage, colorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cmd, outputImage, outputLayout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy cRegion = {};
    cRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cRegion.imageSubresource.layerCount = 1;
    cRegion.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
    vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion);

    transition(cmd, colorImage, colorLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    FfxFsr1DispatchDescription dispatchDesc1 = {};
    dispatchDesc1.commandList = ffxGetCommandListVK(cmd);
    dispatchDesc1.color = colorRes1;
    dispatchDesc1.output = outputRes1;
    dispatchDesc1.renderSize = { (uint32_t)renderW, (uint32_t)renderH };
    dispatchDesc1.enableSharpening = true;
    dispatchDesc1.sharpness = 0.2f;
    ffxFsr1ContextDispatch(fsr1Context, &dispatchDesc1);

    transition(cmd, outputImage, outputLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy outRegion = {};
    outRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outRegion.imageSubresource.layerCount = 1;
    outRegion.imageExtent = { displayW, displayH, 1 };
    vkCmdCopyImageToBuffer(cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);

    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* outData;
    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_1.2_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr1ContextDestroy(fsr1Context);
    _aligned_free(fsr1Context);

    // =========================================================================
    // FSR 2.3.3 TEST
    // =========================================================================
    std::cout << "\n=== FSR 2.3.3 TEST ===" << std::endl; std::cout.flush();
    std::cout << "Initializing FSR 2.3.3 Context..." << std::endl; std::cout.flush();
    memset(scratchBuffer, 0, safeBufferSize);

    FfxInterface ffxInterface2 = {};
    if (ffxGetInterfaceVK(&ffxInterface2, ffxGetDeviceVK(&vkDeviceContext), scratchBuffer, safeBufferSize, 4) != FFX_OK) return 1;

    // --- APPLY OUR C++ INTERCEPTORS HERE ---
    g_original_fpGetDeviceCapabilities = ffxInterface2.fpGetDeviceCapabilities;
    ffxInterface2.fpGetDeviceCapabilities = CustomGetDeviceCapabilities;

    g_original_fpCreateResource = ffxInterface2.fpCreateResource;
    ffxInterface2.fpCreateResource = CustomCreateResource;
    // ---------------------------------------

    FfxFsr2ContextDescription fsr2Desc = {};
    memset(&fsr2Desc, 0, sizeof(FfxFsr2ContextDescription));
    
    fsr2Desc.flags = FFX_FSR2_ENABLE_DEBUG_CHECKING | FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE; 
    
    fsr2Desc.maxRenderSize = { (uint32_t)renderW, (uint32_t)renderH };
    fsr2Desc.displaySize = { displayW, displayH };
    fsr2Desc.fpMessage = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface2;

    FfxFsr2Context* fsr2Context = (FfxFsr2Context*)_aligned_malloc(sizeof(FfxFsr2Context), 64);
    memset(fsr2Context, 0, sizeof(FfxFsr2Context));
    if (ffxFsr2ContextCreate(fsr2Context, &fsr2Desc) != FFX_OK) return 1;

    FfxResource colorRes2 = ffxGetResourceVK(colorImage, ffxGetImageResourceDescriptionVK(colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes2 = ffxGetResourceVK(depthImage, ffxGetImageResourceDescriptionVK(depthImage, depthInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes2 = ffxGetResourceVK(mvImage, ffxGetImageResourceDescriptionVK(mvImage, mvInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"MVs", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes2 = ffxGetResourceVK(outputImage, ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV), L"Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    FfxResource expRes2 = ffxGetResourceVK(expImage, ffxGetImageResourceDescriptionVK(expImage, expInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Exposure", FFX_RESOURCE_STATE_COMPUTE_READ);

    int32_t phaseCount = ffxFsr2GetJitterPhaseCount(renderW, displayW);

    std::cout << "Executing FSR 2.3.3 Temporal Engine (32 Frames)..." << std::endl; std::cout.flush();

    for (int i = 0; i < 32; i++) {
        float jX = 0, jY = 0;
        ffxFsr2GetJitterOffset(&jX, &jY, i, phaseCount);

        std::vector<float> fColor(renderW * renderH * 4);
        std::vector<float> fDepth(renderW * renderH);
        renderScene(renderW, renderH, jX, jY, fColor.data(), fDepth.data());

        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
        memcpy(mappedData, fColor.data(), fColor.size() * sizeof(float));
        memcpy((uint8_t*)mappedData + (fColor.size() * sizeof(float)), fDepth.data(), fDepth.size() * sizeof(float));
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transition(cmd, colorImage, colorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, mvImage, mvLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        
        if (i == 0) {
            transition(cmd, outputImage, outputLayout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        VkBufferImageCopy cRegion2 = {};
        cRegion2.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cRegion2.imageSubresource.layerCount = 1;
        cRegion2.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion2);

        VkBufferImageCopy dRegion = {};
        dRegion.bufferOffset = fColor.size() * sizeof(float);
        dRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dRegion.imageSubresource.layerCount = 1;
        dRegion.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dRegion);

        VkClearColorValue mvClear = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mvClear, 1, &colorRange);

        VkClearColorValue expClear = {{1.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, expImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &expClear, 1, &colorRange);

        transition(cmd, colorImage, colorLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, mvImage, mvLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        FfxFsr2DispatchDescription dispatchDesc = {};
        memset(&dispatchDesc, 0, sizeof(FfxFsr2DispatchDescription)); 
        dispatchDesc.commandList = ffxGetCommandListVK(cmd);
        dispatchDesc.color = colorRes2;
        dispatchDesc.depth = depthRes2;
        dispatchDesc.motionVectors = mvRes2;
        dispatchDesc.exposure = expRes2; 
        dispatchDesc.output = outputRes2;
        dispatchDesc.jitterOffset.x = jX;
        dispatchDesc.jitterOffset.y = jY;
        dispatchDesc.motionVectorScale.x = (float)renderW; 
        dispatchDesc.motionVectorScale.y = (float)renderH;
        dispatchDesc.renderSize = { (uint32_t)renderW, (uint32_t)renderH };
        
        dispatchDesc.enableSharpening = false; // Kept off per your request
        dispatchDesc.sharpness = 0.0f;
        
        dispatchDesc.frameTimeDelta = 16.6f;
        dispatchDesc.preExposure = 1.0f;
        dispatchDesc.reset = (i == 0); 
        dispatchDesc.cameraNear = 0.1f;
        dispatchDesc.cameraFar = 100.0f;
        dispatchDesc.cameraFovAngleVertical = 1.047f;
        dispatchDesc.viewSpaceToMetersFactor = 1.0f;

        if (ffxFsr2ContextDispatch(fsr2Context, &dispatchDesc) != FFX_OK) return 1;

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        if ((i + 1) % 8 == 0) std::cout << " -> Completed Pass " << (i + 1) << "/32" << std::endl;
    }

    std::cout << "Downloading FSR_2.3.3_2x.png..." << std::endl;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);
    transition(cmd, outputImage, outputLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyImageToBuffer(cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);
    vkEndCommandBuffer(cmd);
    
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_2.3.3_2x.png", displayW, displayH, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr2ContextDestroy(fsr2Context);
    _aligned_free(fsr2Context);

    vkDestroyImage(device, colorImage, nullptr); vkFreeMemory(device, colorMem, nullptr);
    vkDestroyImage(device, depthImage, nullptr); vkFreeMemory(device, depthMem, nullptr);
    vkDestroyImage(device, mvImage, nullptr); vkFreeMemory(device, mvMem, nullptr);
    vkDestroyImage(device, outputImage, nullptr); vkFreeMemory(device, outputMem, nullptr);
    vkDestroyImage(device, expImage, nullptr); vkFreeMemory(device, expImageMem, nullptr);
    vkDestroyBuffer(device, uploadBuffer, nullptr); vkFreeMemory(device, uploadMemory, nullptr);
    vkDestroyBuffer(device, downloadBuffer, nullptr); vkFreeMemory(device, downloadMemory, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    _aligned_free(scratchBuffer);

    return 0;
}

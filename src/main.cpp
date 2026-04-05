#define _CRT_SECURE_NO_WARNINGS
#define VOLK_IMPLEMENTATION
#include "volk.h"

#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <iostream>
#include <vector>
#include <malloc.h>
#include <string>
#include <cmath>

// Image libs (Implementation is compiled in stb_impl.cpp!)
#include "stb_image.h"
#include "stb_image_write.h"

static void FfxMessageCallback(FfxMsgType type, const wchar_t* message) {
    if (message) {
        char buffer[2048];
        size_t converted = 0;
        wcstombs_s(&converted, buffer, sizeof(buffer), message, _TRUNCATE);
        std::cout << "[AMD SDK] " << buffer << std::endl;
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

// --- PROCEDURAL 3D RAYTRACER ---
void renderScene(int w, int h, float jx, float jy, unsigned char* colorOut, float* depthOut) {
    float aspect = (float)w / (float)h;
    float fovY = 1.04719755f; // 60 degrees in radians
    float tanHalfFov = tanf(fovY / 2.0f);
    float zNear = 0.1f;
    float zFar = 100.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // NDC space with FSR Sub-Pixel Jitter applied
            float ndcX = ((x + 0.5f + jx) / w) * 2.0f - 1.0f;
            float ndcY = ((y + 0.5f + jy) / h) * 2.0f - 1.0f;

            // Ray Origin & Direction
            float ro[3] = {0.0f, 1.0f, 0.0f};
            float rd[3] = {ndcX * aspect * tanHalfFov, -ndcY * tanHalfFov, 1.0f}; // -ndcY for Vulkan Y-down
            float len = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0]/=len; rd[1]/=len; rd[2]/=len;

            float hitZ = -1.0f;
            unsigned char r = 135, g = 206, b = 235; // Sky Blue background

            // 1. Raycast Sphere at (0, 1, 4) radius 1.5
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
                    
                    float light[3] = {0.577f, 0.577f, -0.577f}; // Directional light
                    float ndotl = fmax(0.2f, -(nx*light[0] + ny*light[1] + nz*light[2])); // Diffuse + Ambient
                    r = (unsigned char)(200 * ndotl);
                    g = (unsigned char)(50 * ndotl);
                    b = (unsigned char)(50 * ndotl);
                }
            }

            // 2. Raycast Checkerboard Floor (y = 0)
            if (rd[1] < 0.0f) {
                float t = -ro[1] / rd[1];
                if (t > zNear && t < zFar && (hitZ < 0.0f || t < hitZ)) {
                    hitZ = t;
                    float px = ro[0] + rd[0]*t;
                    float pz = ro[2] + rd[2]*t;
                    int chk = ((int)floorf(px) + (int)floorf(pz)) % 2;
                    if (chk == 0) { r = 220; g = 220; b = 220; }
                    else { r = 80; g = 80; b = 80; }
                }
            }

            int idx = y * w + x;
            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 255;

            // Calculate True 3D Depth (Standard 0 to 1)
            if (hitZ > 0.0f) {
                // View space Z to Projection space Depth
                float depth = (zFar * (hitZ - zNear)) / (hitZ * (zFar - zNear));
                depthOut[idx] = depth;
            } else {
                depthOut[idx] = 1.0f; // Far Plane
            }
        }
    }
}

// --- VULKAN HELPERS ---
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

void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspectMask) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT; destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

int main() {
    std::cout << "--- FSR 2.3.3 3D Ablation Study ---" << std::endl;

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

    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceInfo.pNext = &features2; 

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

    // --- ABLATION BASELINES ---
    std::cout << "Rendering Native_1x.png (320x240)..." << std::endl;
    std::vector<unsigned char> native1x(renderW * renderH * 4);
    std::vector<float> depth1x(renderW * renderH);
    renderScene(renderW, renderH, 0.0f, 0.0f, native1x.data(), depth1x.data());
    stbi_write_png("Native_1x.png", renderW, renderH, 4, native1x.data(), renderW * 4);

    std::cout << "Rendering Native_2x.png (640x480)..." << std::endl;
    std::vector<unsigned char> native2x(displayW * displayH * 4);
    std::vector<float> depth2x(displayW * displayH);
    renderScene(displayW, displayH, 0.0f, 0.0f, native2x.data(), depth2x.data());
    stbi_write_png("Native_2x.png", displayW, displayH, 4, native2x.data(), displayW * 4);

    // --- ALLOCATE FSR 2.3.3 VRAM BUFFERS ---
    VkDeviceSize uploadSize = (renderW * renderH * 4) + (renderW * renderH * sizeof(float));
    VkBuffer uploadBuffer; VkDeviceMemory uploadMemory;
    createBuffer(device, physicalDevice, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uploadBuffer, uploadMemory);
    
    VkBuffer downloadBuffer; VkDeviceMemory downloadMemory;
    VkDeviceSize outSize = displayW * displayH * 4;
    createBuffer(device, physicalDevice, outSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, downloadBuffer, downloadMemory);

    VkImage colorImage, depthImage, mvImage, outputImage;
    VkDeviceMemory colorMem, depthMem, mvMem, outputMem;
    
    VkImageCreateInfo colorInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, colorImage, colorMem);
    VkImageCreateInfo depthInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, depthImage, depthMem);
    VkImageCreateInfo mvInfo = createImage(device, physicalDevice, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, mvImage, mvMem);
    VkImageCreateInfo outputInfo = createImage(device, physicalDevice, displayW, displayH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, outputImage, outputMem);

    // --- SETUP FSR 2 CONTEXT ---
    std::cout << "Initializing FSR 2.3.3 Context..." << std::endl;
    size_t safeBufferSize = ffxGetScratchMemorySizeVK(physicalDevice, 1) * 2;
    void* scratchBuffer = _aligned_malloc(safeBufferSize, 64);

    VkDeviceContext vkDeviceContext = { device, physicalDevice, vkGetDeviceProcAddr };
    FfxInterface ffxInterface = {};
    ffxGetInterfaceVK(&ffxInterface, ffxGetDeviceVK(&vkDeviceContext), scratchBuffer, safeBufferSize, 1);

    FfxFsr2ContextDescription fsr2Desc = {};
    // Clean SDR Setup
    fsr2Desc.flags = FFX_FSR2_ENABLE_DEBUG_CHECKING; 
    fsr2Desc.maxRenderSize = { (uint32_t)renderW, (uint32_t)renderH };
    fsr2Desc.displaySize = { displayW, displayH };
    fsr2Desc.fpMessage = FfxMessageCallback;
    fsr2Desc.backendInterface = ffxInterface;

    FfxFsr2Context fsr2Context;
    if (ffxFsr2ContextCreate(&fsr2Context, &fsr2Desc) != FFX_OK) return 1;

    FfxResource colorRes = ffxGetResourceVK(colorImage, ffxGetImageResourceDescriptionVK(colorImage, colorInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource depthRes = ffxGetResourceVK(depthImage, ffxGetImageResourceDescriptionVK(depthImage, depthInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource mvRes = ffxGetResourceVK(mvImage, ffxGetImageResourceDescriptionVK(mvImage, mvInfo, FFX_RESOURCE_USAGE_READ_ONLY), L"MVs", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outputRes = ffxGetResourceVK(outputImage, ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV), L"Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    int32_t phaseCount = ffxFsr2GetJitterPhaseCount(renderW, displayW);

    std::cout << "Executing FSR 2.3.3 Temporal Engine (32 Frames)..." << std::endl;

    for (int i = 0; i < 32; i++) {
        // 1. Generate Sub-Pixel Jitter
        float jX = 0, jY = 0;
        ffxFsr2GetJitterOffset(&jX, &jY, i, phaseCount);

        // 2. Render CPU 3D Scene
        std::vector<unsigned char> fColor(renderW * renderH * 4);
        std::vector<float> fDepth(renderW * renderH);
        renderScene(renderW, renderH, jX, jY, fColor.data(), fDepth.data());

        // 3. Upload to Vulkan
        void* mappedData;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mappedData);
        memcpy(mappedData, fColor.data(), fColor.size());
        memcpy((uint8_t*)mappedData + fColor.size(), fDepth.data(), fDepth.size() * sizeof(float));
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transitionImageLayout(cmd, colorImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transitionImageLayout(cmd, depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transitionImageLayout(cmd, mvImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        if (i == 0) transitionImageLayout(cmd, outputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cRegion = {};
        cRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cRegion.imageSubresource.layerCount = 1;
        cRegion.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cRegion);

        VkBufferImageCopy dRegion = {};
        dRegion.bufferOffset = fColor.size();
        dRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dRegion.imageSubresource.layerCount = 1;
        dRegion.imageExtent = { (uint32_t)renderW, (uint32_t)renderH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dRegion);

        // Motion vectors are strictly ZERO because the camera is not moving!
        VkClearColorValue mvClear = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange mvRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mvClear, 1, &mvRange);

        transitionImageLayout(cmd, colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transitionImageLayout(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transitionImageLayout(cmd, mvImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // 4. Dispatch FSR
        FfxFsr2DispatchDescription dispatchDesc = {};
        dispatchDesc.commandList = ffxGetCommandListVK(cmd);
        dispatchDesc.color = colorRes;
        dispatchDesc.depth = depthRes;
        dispatchDesc.motionVectors = mvRes;
        dispatchDesc.output = outputRes;
        
        dispatchDesc.jitterOffset.x = jX;
        dispatchDesc.jitterOffset.y = jY;
        dispatchDesc.motionVectorScale.x = (float)renderW; 
        dispatchDesc.motionVectorScale.y = (float)renderH;
        dispatchDesc.renderSize = { (uint32_t)renderW, (uint32_t)renderH };
        dispatchDesc.enableSharpening = true;
        dispatchDesc.sharpness = 0.2f;
        dispatchDesc.frameTimeDelta = 16.6f;
        dispatchDesc.preExposure = 1.0f;
        dispatchDesc.reset = (i == 0); 
        dispatchDesc.cameraNear = 0.1f;
        dispatchDesc.cameraFar = 100.0f;
        dispatchDesc.cameraFovAngleVertical = 1.047f; // 60 degrees

        if (ffxFsr2ContextDispatch(&fsr2Context, &dispatchDesc) != FFX_OK) {
            std::cout << "FAILED: ffxFsr2ContextDispatch failed on loop " << i << std::endl;
            return 1;
        }

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, nullptr, nullptr, 1, &cmd };
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        if ((i + 1) % 8 == 0) std::cout << "  -> Completed Pass " << (i + 1) << "/32" << std::endl;
    }

    // --- DOWNLOAD RESULT ---
    std::cout << "Downloading FSR_2.3.3_2x.png..." << std::endl;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transitionImageLayout(cmd, outputImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy outRegion = {};
    outRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outRegion.imageSubresource.layerCount = 1;
    outRegion.imageExtent = { displayW, displayH, 1 };
    
    vkCmdCopyImageToBuffer(cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, downloadBuffer, 1, &outRegion);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, nullptr, nullptr, 1, &cmd };
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* outData;
    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    stbi_write_png("FSR_2.3.3_2x.png", displayW, displayH, 4, outData, displayW * 4);
    vkUnmapMemory(device, downloadMemory);
    std::cout << "Done!" << std::endl;

    return 0;
}

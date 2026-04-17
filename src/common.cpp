#include "common.h"

uint32_t appFindMemoryType(VkPhysicalDevice physicalDevice,
                           uint32_t typeFilter,
                           VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeFilter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return 0;
}

void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bi, nullptr, &buffer);

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buffer, &mr);

    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = appFindMemoryType(physicalDevice,
                                           mr.memoryTypeBits, properties);
    vkAllocateMemory(device, &ai, nullptr, &bufferMemory);
    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

VkImageCreateInfo createImage(VkDevice device, VkPhysicalDevice physicalDevice,
                              uint32_t width, uint32_t height,
                              VkFormat format, VkImageUsageFlags usage,
                              VkImage& image, VkDeviceMemory& imageMemory)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.extent        = { width, height, 1 };
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.format        = format;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage         = usage;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &ii, nullptr, &image);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);

    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = appFindMemoryType(physicalDevice, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &ai, nullptr, &imageMemory);
    vkBindImageMemory(device, image, imageMemory, 0);
    return ii;
}

void transition(VkCommandBuffer cmd, VkImage image,
                VkImageLayout& currentLayout, VkImageLayout newLayout,
                VkImageAspectFlags aspectMask, uint32_t mipLevels)
{
    if (currentLayout == newLayout) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout           = currentLayout;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { aspectMask, 0, mipLevels, 0, 1 };
    b.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
    currentLayout = newLayout;
}

// ---------- FFX format helpers -----------------------------------------------
VkFormat ffxFormatToVk(FfxSurfaceFormat fmt)
{
    switch (fmt) {
    case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32B32A32_UINT:     return VK_FORMAT_R32G32B32A32_UINT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32_FLOAT:          return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32_TYPELESS:       return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R8_UINT:               return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R32_UINT:              return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:        return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB:         return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT:       return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT:          return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_UINT:           return VK_FORMAT_R16G16_UINT;
    case FFX_SURFACE_FORMAT_R16G16_SINT:           return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT:             return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16_UINT:              return VK_FORMAT_R16_UINT;
    case FFX_SURFACE_FORMAT_R16_UNORM:             return VK_FORMAT_R16_UNORM;
    case FFX_SURFACE_FORMAT_R16_SNORM:             return VK_FORMAT_R16_SNORM;
    case FFX_SURFACE_FORMAT_R8_UNORM:              return VK_FORMAT_R8_UNORM;
    case FFX_SURFACE_FORMAT_R32_FLOAT:             return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R8G8_UNORM:            return VK_FORMAT_R8G8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8_UINT:             return VK_FORMAT_R8G8_UINT;
    case FFX_SURFACE_FORMAT_R32G32B32_FLOAT:       return VK_FORMAT_R32G32B32_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_TYPELESS: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM:     return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:    return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default:
        std::cout << "[WARN] Unknown FfxSurfaceFormat " << (int)fmt
                  << ", falling back to R32_SFLOAT\n";
        return VK_FORMAT_R32_SFLOAT;
    }
}

VkImageUsageFlags ffxUsageToVk(FfxResourceUsage usage)
{
    VkImageUsageFlags f =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & FFX_RESOURCE_USAGE_UAV)          f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & FFX_RESOURCE_USAGE_RENDERTARGET) f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return f;
}

SharedImage createSharedImage(VkDevice device, VkPhysicalDevice physicalDevice,
                              const FfxCreateResourceDescription& desc)
{
    SharedImage si;
    VkFormat          fmt   = ffxFormatToVk(desc.resourceDescription.format);
    VkImageUsageFlags usage = ffxUsageToVk(desc.resourceDescription.usage);

    uint32_t w    = desc.resourceDescription.width;
    uint32_t h    = desc.resourceDescription.height;
    uint32_t mips = desc.resourceDescription.mipCount;
    if (mips == 0) {
        mips = 1;
        uint32_t tw = w, th = h;
        while (tw > 1 || th > 1) { tw >>= 1; th >>= 1; ++mips; }
    }

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.extent        = { w, h, 1 };
    ii.mipLevels     = mips;
    ii.arrayLayers   = 1;
    ii.format        = fmt;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage         = usage;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateImage(device, &ii, nullptr, &si.image);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, si.image, &mr);

    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = appFindMemoryType(physicalDevice, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &ai, nullptr, &si.memory);
    vkBindImageMemory(device, si.image, si.memory, 0);

    si.info   = ii;
    si.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return si;
}

// =============================================================================
//  renderScene
//
//  Cauldron-faithful ray-cast renderer.
//
//  JITTER:
//    jX, jY come from ffxFsr3UpscalerGetJitterOffset (or ffxFsr2GetJitterOffset).
//    These are in unit-pixel space (Halton[2,3], range ~[-0.5, +0.5]).
//    To apply as a projection-matrix jitter:
//      ndcJX =  2 * jX / renderWidth
//      ndcJY = -2 * jY / renderHeight   (Y negated for NDC convention)
//    These are added to the pixel's base NDC coordinate before ray construction.
//
//  MOTION VECTORS (Cauldron convention, with JITTER_CANCELLATION):
//    Output in PIXEL space (screen-space deltas).
//    For a static camera + static scene, the only motion is from jitter.
//    Cauldron bakes the jitter delta into the MV:
//      mv.x = prevJX - currJX   (pixel space)
//      mv.y = prevJY - currJY   (pixel space)
//    This is passed with FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION
//    so FSR subtracts the jitter component and recovers true zero motion.
//    motionVectorScale = {1, 1} because MVs are already in pixel space.
//
//    NOTE: "pixel space" here means values are in the [-renderW, renderW] range
//    that FSR expects. The jitter delta is tiny (sub-pixel), which is correct.
//
//  DEPTH (reversed-Z + infinite far, matching Cauldron):
//    depth = zNear / z   (larger value = closer)
//    Background pixels: depth = 0.
//    Flags: DEPTH_INVERTED | DEPTH_INFINITE
//    Dispatch: cameraNear = FLT_MAX, cameraFar = CAM_Z_NEAR (0.1)
//    (Cauldron convention for infinite reversed-Z)
//
// =============================================================================
void renderScene(int w, int h,
                 float currJX, float currJY,
                 float prevJX, float prevJY,
                 float* colorOut, float* depthOut, float* mvOut)
{
    const float aspect     = (float)w / (float)h;
    const float tanHalfFov = tanf(CAM_FOV_Y * 0.5f);

    // Convert jitter from unit-pixel-space to NDC-space offsets.
    // This is what a projection matrix jitter does.
    const float currNdcJX =  2.0f * currJX / (float)w;
    const float currNdcJY = -2.0f * currJY / (float)h;

    // Camera origin (fixed)
    const float rox = 0.f, roy = 1.f, roz = 0.f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {

            // Build jittered ray for this pixel.
            float ndcX = ((x + 0.5f) / (float)w) * 2.0f - 1.0f + currNdcJX;
            float ndcY = ((y + 0.5f) / (float)h) * 2.0f - 1.0f + currNdcJY;

            float rdx =  ndcX * aspect * tanHalfFov;
            float rdy = -ndcY * tanHalfFov;
            float rdz =  1.0f;
            float rlen = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);
            rdx /= rlen; rdy /= rlen; rdz /= rlen;

            float hitT = -1.f;
            float r = powf(135.f/255.f, 2.2f);
            float g = powf(206.f/255.f, 2.2f);
            float b = powf(235.f/255.f, 2.2f);

            // Sphere at (0, 1, 4), radius 1.5
            {
                float ocx = rox, ocy = roy - 1.f, ocz = roz - 4.f;
                float bd   = rdx*ocx + rdy*ocy + rdz*ocz;
                float cv   = ocx*ocx + ocy*ocy + ocz*ocz - 2.25f;
                float disc = bd*bd - cv;
                if (disc > 0.f) {
                    float t = -bd - sqrtf(disc);
                    if (t > CAM_Z_NEAR && t < CAM_Z_FAR) {
                        hitT = t;
                        float nx = rox + rdx*t;
                        float ny = roy + rdy*t - 1.f;
                        float nz = roz + rdz*t - 4.f;
                        float nl = sqrtf(nx*nx + ny*ny + nz*nz);
                        nx/=nl; ny/=nl; nz/=nl;
                        float lx = 0.577f, ly = 0.577f, lz = -0.577f;
                        float ndotl = fmaxf(0.2f, -(nx*lx + ny*ly + nz*lz));
                        r = powf(200.f/255.f, 2.2f) * ndotl;
                        g = powf( 50.f/255.f, 2.2f) * ndotl;
                        b = powf( 50.f/255.f, 2.2f) * ndotl;
                    }
                }
            }

            // Floor at y=0
            if (rdy < 0.f) {
                float t = -roy / rdy;
                if (t > CAM_Z_NEAR && t < CAM_Z_FAR && (hitT < 0.f || t < hitT)) {
                    hitT = t;
                    float px = rox + rdx*t;
                    float pz = roz + rdz*t;
                    int chk = ((int)floorf(px) + (int)floorf(pz)) & 1;
                    float cv2 = (chk == 0)
                        ? powf(220.f/255.f, 2.2f)
                        : powf( 80.f/255.f, 2.2f);
                    r = g = b = cv2;
                }
            }

            int idx = y * w + x;

            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 1.f;

            // Reversed-Z depth: zNear/z. Background = 0.
            // Used with DEPTH_INVERTED | DEPTH_INFINITE.
            // Dispatch: cameraNear = FLT_MAX, cameraFar = CAM_Z_NEAR.
            depthOut[idx] = (hitT > 0.f) ? (CAM_Z_NEAR / hitT) : 0.f;

            // Motion vectors in pixel space (Cauldron convention).
            // For a static scene, the only per-frame delta is from jitter.
            // We bake the jitter delta in so FSR can verify temporal consistency
            // after removing it via MOTION_VECTORS_JITTER_CANCELLATION.
            // mv = prevJitter - currJitter  (pixel space, tiny sub-pixel values)
            mvOut[idx*2+0] = prevJX - currJX;
            mvOut[idx*2+1] = prevJY - currJY;
        }
    }
}

// ---------- I/O helpers -------------------------------------------------------
void saveFloatImage(const std::string& filename, int w, int h,
                    const float* data)
{
    std::vector<unsigned char> bytes(w * h * 4);
    int nanCount = 0;
    for (int i = 0; i < w*h*4; i += 4) {
        for (int c = 0; c < 3; c++) {
            float v = data[i+c];
            if (std::isnan(v)) { v = 0.f; ++nanCount; }
            v = fmaxf(0.f, fminf(1.f, v));
            bytes[i+c] = (unsigned char)(powf(v, 1.f/2.2f) * 255.f);
        }
        bytes[i+3] = 255;
    }
    if (nanCount > 0)
        std::cout << "[WARNING] " << nanCount << " NaNs in " << filename << "\n";
    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w*4);
    std::cout << "Saved: " << filename << "\n";
    std::cout.flush();
}

void saveDepthImage(const std::string& filename, int w, int h,
                    const float* data)
{
    std::vector<unsigned char> bytes(w * h * 4);
    float minV = 1e30f, maxV = -1e30f;
    for (int i = 0; i < w*h; i++) {
        float v = data[i];
        if (!std::isnan(v) && !std::isinf(v)) {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }
    std::cout << "[Depth] min=" << minV << " max=" << maxV << "\n";
    float range = (maxV > minV) ? (maxV - minV) : 1.f;
    for (int i = 0; i < w*h; i++) {
        float v = data[i];
        if (std::isnan(v)) v = 0.f;
        v = fmaxf(0.f, fminf(1.f, (v - minV) / range));
        unsigned char bv = (unsigned char)(v * 255.f);
        bytes[i*4+0] = bytes[i*4+1] = bytes[i*4+2] = bv;
        bytes[i*4+3] = 255;
    }
    stbi_write_png(filename.c_str(), w, h, 4, bytes.data(), w*4);
    std::cout << "Saved: " << filename << "\n";
    std::cout.flush();
}

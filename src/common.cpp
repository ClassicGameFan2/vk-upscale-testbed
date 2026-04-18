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

// ---------- Scene / I/O ------------------------------------------------------
//
// renderScene — correct Cauldron-style implementation:
//
//  JITTER
//  ------
//  jX, jY are in UNIT PIXEL SPACE as returned by ffxFsr*GetJitterOffset.
//  They implement a Halton(2,3) sequence in [-0.5, +0.5].
//  To apply jitter we shift the ray origin in NDC:
//      ndcJX = jX * 2.0f / w
//      ndcJY = jY * 2.0f / h
//  Color and Depth are rendered WITH this jitter shift (the scene is jittered).
//
//  MOTION VECTORS
//  --------------
//  For a STATIC scene with a STATIC camera, motion vectors are always (0, 0).
//  Jitter delta must NOT appear in the motion vectors.
//  Do NOT set FFX_FSR*_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION.
//
//  DEPTH
//  -----
//  We use reversed-Z with a finite far plane (SCENE_ZFAR = 50.0f).
//  Formula: depth = zNear / hitT   => 1.0 at the near plane, small at far.
//  Background (sky, no geometry hit, or hit beyond zFar) => depth = 0.0.
//  FSR flags: DEPTH_INVERTED only (NOT DEPTH_INFINITE, because zFar is finite).
//  Dispatch: cameraNear = zNear (0.1f), cameraFar = SCENE_ZFAR (50.0f).
//
//  HORIZON
//  -------
//  The floor is clipped at SCENE_ZFAR to prevent the horizon aliasing caused
//  by the checkerboard tile frequency becoming infinite at grazing angles.
//  A smooth fog fade blends the floor to sky colour over the last 40% of the
//  view distance so there is no hard cutoff line.
//
// NOTE: SCENE_ZFAR is published in common.h so the pass files can use it
//       for cameraNear/cameraFar.
//
void renderScene(int w, int h,
                 float jX, float jY,
                 float* colorOut,
                 float* depthOut,
                 float* mvOut)
{
    const float aspect     = (float)w / (float)h;
    const float fovY       = 1.04719755f;   // 60 degrees
    const float tanHalfFov = tanf(fovY * 0.5f);

    // Convert pixel-space jitter to NDC-space offset.
    // ffxFsr*GetJitterOffset returns values in unit pixel space [-0.5, +0.5].
    // One pixel = 2/w in NDC, so:
    const float ndcJX = jX * 2.0f / (float)w;
    const float ndcJY = jY * 2.0f / (float)h;

    // Pre-compute linear sky colour (used for background and fog blend).
    const float skyR = powf(135.f / 255.f, 2.2f);
    const float skyG = powf(206.f / 255.f, 2.2f);
    const float skyB = powf(235.f / 255.f, 2.2f);

    // Fog band: blend floor to sky over the last FOG_FRACTION of zFar.
    // e.g. 0.4 means fog starts at 0.6 * zFar and is complete at zFar.
    const float FOG_START = SCENE_ZFAR * 0.6f;
    const float FOG_RANGE = SCENE_ZFAR - FOG_START; // = SCENE_ZFAR * 0.4f

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // Pixel centre in NDC, with jitter shift applied.
            float ndcX = ((x + 0.5f) / w) * 2.0f - 1.0f + ndcJX;
            float ndcY = ((y + 0.5f) / h) * 2.0f - 1.0f + ndcJY;

            // Ray origin and direction (view space, looking down +Z).
            float ro[3] = { 0.f, 1.f, 0.f };
            float rd[3] = { ndcX * aspect * tanHalfFov,
                           -ndcY * tanHalfFov,
                            1.f };
            float len = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0] /= len;  rd[1] /= len;  rd[2] /= len;

            // Start with sky colour and no hit.
            float r = skyR, g = skyG, b = skyB;
            float hitZ = -1.f;   // negative means no hit yet

            // ── Sphere at (0, 1, 4), radius 1.5 ────────────────────────────
            {
                float oc[3] = { ro[0], ro[1] - 1.f, ro[2] - 4.f };
                float bd   = rd[0]*oc[0] + rd[1]*oc[1] + rd[2]*oc[2];
                float cv   = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - 2.25f;
                float disc = bd*bd - cv;
                if (disc > 0.f) {
                    float t = -bd - sqrtf(disc);
                    if (t > SCENE_ZNEAR && t < SCENE_ZFAR) {
                        hitZ = t;
                        float nx = ro[0] + rd[0]*t;
                        float ny = ro[1] + rd[1]*t - 1.f;
                        float nz = ro[2] + rd[2]*t - 4.f;
                        float nl = sqrtf(nx*nx + ny*ny + nz*nz);
                        nx /= nl;  ny /= nl;  nz /= nl;
                        float light[3] = { 0.577f, 0.577f, -0.577f };
                        float ndotl = fmaxf(0.2f,
                            -(nx*light[0] + ny*light[1] + nz*light[2]));
                        r = powf(200.f / 255.f, 2.2f) * ndotl;
                        g = powf( 50.f / 255.f, 2.2f) * ndotl;
                        b = powf( 50.f / 255.f, 2.2f) * ndotl;
                    }
                }
            }

            // ── Infinite floor at y = 0 ─────────────────────────────────────
            // Clamp to SCENE_ZFAR to prevent horizon aliasing:
            //   At grazing angles, t = ro[1] / |rd[1]| grows without bound.
            //   floorf(px) and floorf(pz) then oscillate wildly between tile
            //   edges due to floating-point imprecision, producing a jagged
            //   checker line at the horizon.
            // Fix: ignore floor hits beyond SCENE_ZFAR.
            // Smooth fix: fade floor colour to sky over the fog band so the
            //   clip boundary is invisible.
            if (rd[1] < 0.f) {
                float t = -ro[1] / rd[1];
                if (t > SCENE_ZNEAR && t < SCENE_ZFAR &&
                    (hitZ < 0.f || t < hitZ))
                {
                    hitZ = t;
                    float px  = ro[0] + rd[0]*t;
                    float pz  = ro[2] + rd[2]*t;
                    int   chk = ((int)floorf(px) + (int)floorf(pz)) & 1;
                    float cv2 = (chk == 0) ? powf(220.f / 255.f, 2.2f)
                                           : powf( 80.f / 255.f, 2.2f);

                    // Fog blend: alpha = 1 (full floor) near camera,
                    //            alpha = 0 (full sky)  at SCENE_ZFAR.
                    float fogAlpha = 1.f;
                    if (t > FOG_START)
                        fogAlpha = fmaxf(0.f,
                            1.f - (t - FOG_START) / FOG_RANGE);

                    r = cv2 * fogAlpha + skyR * (1.f - fogAlpha);
                    g = cv2 * fogAlpha + skyG * (1.f - fogAlpha);
                    b = cv2 * fogAlpha + skyB * (1.f - fogAlpha);
                }
            }

            int idx = y * w + x;

            // Color (jittered scene)
            colorOut[idx*4 + 0] = r;
            colorOut[idx*4 + 1] = g;
            colorOut[idx*4 + 2] = b;
            colorOut[idx*4 + 3] = 1.f;

            // Depth: reversed-Z, finite far plane.
            //   hitT = SCENE_ZNEAR  =>  depth = 1.0  (near plane)
            //   hitT = SCENE_ZFAR   =>  depth = SCENE_ZNEAR / SCENE_ZFAR  (~0.002)
            //   no hit (sky)        =>  depth = 0.0  (beyond far plane)
            // FSR flags: DEPTH_INVERTED only (NOT DEPTH_INFINITE).
            // Dispatch: cameraNear = SCENE_ZNEAR, cameraFar = SCENE_ZFAR.
            depthOut[idx] = (hitZ > 0.f) ? (SCENE_ZNEAR / hitZ) : 0.f;

            // Motion Vectors: always zero.
            // The scene and camera are static. Jitter must NOT appear in MVs.
            mvOut[idx*2 + 0] = 0.f;
            mvOut[idx*2 + 1] = 0.f;
        }
    }
}

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

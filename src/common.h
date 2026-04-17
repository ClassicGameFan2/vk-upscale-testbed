#pragma once
#define _CRT_SECURE_NO_WARNINGS
// VOLK_IMPLEMENTATION defined in main.cpp only
#include "volk.h"

#include <FidelityFX/host/ffx_fsr1.h>
#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <cfloat>
#include <malloc.h>
#include "stb_image_write.h"

static constexpr uint32_t RENDER_W  = 320;
static constexpr uint32_t RENDER_H  = 240;
static constexpr uint32_t DISPLAY_W = 640;
static constexpr uint32_t DISPLAY_H = 480;

// Camera constants shared between renderScene and the FSR passes
static constexpr float CAM_FOV_Y   = 1.04719755f; // 60 degrees
static constexpr float CAM_Z_NEAR  = 0.1f;
static constexpr float CAM_Z_FAR   = 100.0f;

// ---------- Vulkan helpers ---------------------------------------------------
uint32_t appFindMemoryType(VkPhysicalDevice physicalDevice,
                           uint32_t typeFilter,
                           VkMemoryPropertyFlags properties);

void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory);

VkImageCreateInfo createImage(VkDevice device, VkPhysicalDevice physicalDevice,
                              uint32_t width, uint32_t height,
                              VkFormat format, VkImageUsageFlags usage,
                              VkImage& image, VkDeviceMemory& imageMemory);

void transition(VkCommandBuffer cmd, VkImage image,
                VkImageLayout& currentLayout, VkImageLayout newLayout,
                VkImageAspectFlags aspectMask, uint32_t mipLevels = 1);

// ---------- FFX helpers ------------------------------------------------------
VkFormat          ffxFormatToVk(FfxSurfaceFormat fmt);
VkImageUsageFlags ffxUsageToVk(FfxResourceUsage usage);

struct SharedImage {
    VkImage           image  = VK_NULL_HANDLE;
    VkDeviceMemory    memory = VK_NULL_HANDLE;
    VkImageCreateInfo info   = {};
    VkImageLayout     layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

SharedImage createSharedImage(VkDevice device, VkPhysicalDevice physicalDevice,
                              const FfxCreateResourceDescription& desc);

// ---------- Scene / I/O ------------------------------------------------------
//
// Jitter parameters (currJX/currJY, prevJX/prevJY) are in UNIT PIXEL SPACE
// exactly as returned by ffxFsr3UpscalerGetJitterOffset / ffxFsr2GetJitterOffset.
//
// The function internally converts them to NDC offsets before applying to rays,
// and computes motion vectors as true render-resolution pixel-space reprojection
// deltas (NOT jitter deltas).
//
// colorOut : RENDER_W * RENDER_H * 4 floats (RGBA, linear)
// depthOut : RENDER_W * RENDER_H * 1 float  (reversed-Z: zNear/z, [0..1])
// mvOut    : RENDER_W * RENDER_H * 2 floats (pixel-space, render resolution)
//            mv.x = screen_x_prev - screen_x_curr  (horizontal displacement)
//            mv.y = screen_y_prev - screen_y_curr  (vertical  displacement)
//            For a static scene with no camera movement: all zeros.
//            Jitter is NOT baked into the motion vectors.
//
void renderScene(int w, int h,
                 float currJX, float currJY,   // unit pixel space
                 float prevJX, float prevJY,   // unit pixel space (previous frame)
                 float* colorOut,
                 float* depthOut,
                 float* mvOut);

void saveFloatImage(const std::string& filename, int w, int h,
                    const float* data);
void saveDepthImage(const std::string& filename, int w, int h,
                    const float* data);

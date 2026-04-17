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
// jX, jY  : pixel-space jitter for this frame (from ffxFsr*GetJitterOffset).
//            These are applied to color + depth only.
//            Motion vectors are always zero (static scene, static camera,
//            no jitter in MVs -- do NOT set JITTER_CANCELLATION flag).
void renderScene(int w, int h,
                 float jX, float jY,
                 float* colorOut, float* depthOut, float* mvOut);

void saveFloatImage(const std::string& filename, int w, int h,
                    const float* data);
void saveDepthImage(const std::string& filename, int w, int h,
                    const float* data);

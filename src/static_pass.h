#pragma once
#include "common.h"
#include "settings.h"

// ============================================================================
// RunStaticPngPass
//
// Reads a PNG file, applies FSR1/2/3 upscaling and/or RCAS sharpening
// according to the settings, and writes the result as a PNG.
// ============================================================================
void RunStaticPngPass(
    VkDevice            device,
    VkPhysicalDevice    physicalDevice,
    VkQueue             queue,
    VkCommandBuffer     cmd,
    VkDeviceContext&    vkDevCtx,
    const AppSettings&  cfg);

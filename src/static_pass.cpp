// =============================================================================
//  static_pass.cpp
//
//  Applies FSR1 / FSR2 / FSR3 upscaling (and/or RCAS) to a PNG file.
//
//  Pipeline per frame:
//    1.  Load the input PNG (once, at the start).
//    2.  Compute the jitter offset for this frame using the FSR-API jitter
//        sequence.
//    3.  Resample the PNG with the jitter offset to produce a per-frame input
//        (bilinear / bicubic / lanczos3).
//    4.  Fill a flat depth buffer (uniform Static_Depth value) for FSR2/FSR3.
//    5.  Fill motion-vectors encoding the jitter delta (prevJitter - currJitter)
//        so that FSR2/3 can remove the sub-pixel camera shift via its
//        JITTER_CANCELLATION path.
//    6.  Upload colour / depth / MV to Vulkan images.
//    7.  Dispatch FSR.
//    8.  On the final frame, readback and save the result PNG.
//
//  For FSR1 (spatial only):
//    - Jitter and temporal accumulation do not apply.
//    - Only 1 frame is dispatched regardless of Static_Jitter_Frames.
//    - enableSharpening controls RCAS.
//
//  Depth convention (static pass ONLY):
//    Standard (NON-inverted) depth: 0 = camera / near, 1 = far / background.
//    Static_Depth default is 0.5 (mid-range, physically neutral).
//    No DEPTH_INVERTED flag is set for FSR2/FSR3 in this pass.
//
//  Camera near/far (static pass ONLY):
//    cameraNear = 0.1   cameraFar = 100.0  (standard perspective range).
//
//  Exposure (static pass ONLY):
//    AUTO_EXPOSURE is intentionally disabled for FSR2 and FSR3.
//    AUTO_EXPOSURE runs FSR's SPD luminance-pyramid pass each frame to
//    compute average scene luminance.  For a static image whose jittered
//    frames differ only at sub-pixel level, the SPD result fluctuates
//    slightly between frames.  FSR2/3 interpret that fluctuation as content
//    change, reduce history confidence in high-contrast regions, and produce
//    a visible "thumb smear" artifact in the center of the output.
//    Using a fixed preExposure = 1.0 avoids this entirely.
// =============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include "static_pass.h"
#include "jitter_resample.h"

#include "stb_image.h"

#include <vector>
#include <string>
#include <iostream>
#include <cmath>
#include <cfloat>
#include <malloc.h>

// ---------------------------------------------------------------------------
// Message callback shared by FSR2/FSR3 in this pass
// ---------------------------------------------------------------------------
static void StaticFfxMsg(FfxMsgType /*type*/, const wchar_t* message)
{
    if (!message) return;
    std::cout << "[Static FSR MSG] ";
    for (int i = 0; i < 2048 && message[i]; ++i)
        std::cout << (char)message[i];
    std::cout << "\n";
    std::cout.flush();
}

// ===========================================================================
// Helpers to allocate / free a Vulkan image + device memory
// ===========================================================================

struct TempImage {
    VkImage           image  = VK_NULL_HANDLE;
    VkDeviceMemory    memory = VK_NULL_HANDLE;
    VkImageCreateInfo info   = {};
    VkImageLayout     layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

static TempImage makeTempImage(
    VkDevice device, VkPhysicalDevice physDev,
    uint32_t w, uint32_t h,
    VkFormat fmt, VkImageUsageFlags usage)
{
    TempImage ti;
    ti.info = createImage(device, physDev, w, h, fmt, usage, ti.image, ti.memory);
    ti.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ti;
}

static void freeTempImage(VkDevice device, TempImage& ti)
{
    if (ti.image  != VK_NULL_HANDLE) { vkDestroyImage(device, ti.image,  nullptr); ti.image  = VK_NULL_HANDLE; }
    if (ti.memory != VK_NULL_HANDLE) { vkFreeMemory  (device, ti.memory, nullptr); ti.memory = VK_NULL_HANDLE; }
}

// ===========================================================================
// Readback output image → float buffer
// ===========================================================================

static void readbackImage(
    VkDevice device,
    VkQueue queue, VkCommandBuffer cmd,
    VkBuffer dlBuf, VkDeviceMemory dlMem,
    VkImage srcImage, VkImageLayout& srcLayout,
    VkDeviceSize outSize, uint32_t w, uint32_t h,
    std::vector<float>& outData)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bi);
    transition(cmd, srcImage, srcLayout,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy cr = {};
    cr.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cr.imageExtent = { w, h, 1 };
    vkCmdCopyImageToBuffer(cmd, srcImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dlBuf, 1, &cr);
    // Return to GENERAL so we can keep rendering into it
    transition(cmd, srcImage, srcLayout,
               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* mp;
    vkMapMemory(device, dlMem, 0, outSize, 0, &mp);
    outData.assign((float*)mp, (float*)mp + outSize / sizeof(float));
    vkUnmapMemory(device, dlMem);
}

// ===========================================================================
//  FSR-1 static pass
//  (unchanged from previous version — no AUTO_EXPOSURE issue here)
// ===========================================================================

static void RunStaticFsr1(
    VkDevice device, VkPhysicalDevice physDev,
    VkQueue queue, VkCommandBuffer cmd,
    VkDeviceContext& vkDevCtx,
    const AppSettings& cfg,
    const std::vector<float>& inputLinear,
    int inW, int inH,
    int outW, int outH,
    const std::string& outPath)
{
    std::cout << "\n[StaticFSR1] -------- FSR 1.2 Static PNG Pass --------\n";
    std::cout << "[StaticFSR1] Input:  " << inW  << "x" << inH  << "\n";
    std::cout << "[StaticFSR1] Output: " << outW << "x" << outH << "\n";
    std::cout << "[StaticFSR1] RCAS:   " << (cfg.static_rcas ? "ON" : "OFF")
              << "  sharpness=" << cfg.static_sharpness << "\n";

    VkDeviceSize colorUploadSize = (VkDeviceSize)(inW * inH * 4 * sizeof(float));
    VkDeviceSize outSize         = (VkDeviceSize)(outW * outH * 4 * sizeof(float));

    VkBuffer uploadBuf; VkDeviceMemory uploadMem;
    createBuffer(device, physDev, colorUploadSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 uploadBuf, uploadMem);

    VkBuffer dlBuf; VkDeviceMemory dlMem;
    createBuffer(device, physDev, outSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 dlBuf, dlMem);

    TempImage colorImg = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage outImg   = makeTempImage(device, physDev, (uint32_t)outW, (uint32_t)outH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT      | VK_IMAGE_USAGE_SAMPLED_BIT);

    size_t scratchSize = ffxGetScratchMemorySizeVK(physDev, 4);
    void*  scratch     = _aligned_malloc(scratchSize, 64);
    memset(scratch, 0, scratchSize);

    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratch, scratchSize, 4);

    FfxFsr1ContextDescription desc = {};
    desc.flags          = FFX_FSR1_ENABLE_HIGH_DYNAMIC_RANGE;
    if (cfg.static_rcas) desc.flags |= FFX_FSR1_ENABLE_RCAS;
    desc.outputFormat   = ffxGetSurfaceFormatVK(VK_FORMAT_R32G32B32A32_SFLOAT);
    desc.maxRenderSize  = { (uint32_t)inW,  (uint32_t)inH  };
    desc.displaySize    = { (uint32_t)outW, (uint32_t)outH };
    desc.backendInterface = ffxIface;

    FfxFsr1Context* ctx =
        (FfxFsr1Context*)_aligned_malloc(sizeof(FfxFsr1Context), 64);
    memset(ctx, 0, sizeof(FfxFsr1Context));
    if (ffxFsr1ContextCreate(ctx, &desc) != FFX_OK) {
        std::cout << "[StaticFSR1] FATAL: ffxFsr1ContextCreate failed!\n";
        goto cleanup_fsr1;
    }
    std::cout << "[StaticFSR1] Context created.\n";

    {
        void* mp;
        vkMapMemory(device, uploadMem, 0, colorUploadSize, 0, &mp);
        memcpy(mp, inputLinear.data(), colorUploadSize);
        vkUnmapMemory(device, uploadMem);
    }

    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);
        transition(cmd, outImg.image, outImg.layout,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);

        transition(cmd, colorImg.image, colorImg.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cr = {};
        cr.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cr.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuf, colorImg.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cr);

        transition(cmd, colorImg.image, colorImg.layout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        FfxResourceDescription colorFfxDesc =
            ffxGetImageResourceDescriptionVK(colorImg.image, colorImg.info,
                                              FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription outFfxDesc =
            ffxGetImageResourceDescriptionVK(outImg.image, outImg.info,
                                              FFX_RESOURCE_USAGE_UAV);

        FfxResource colorRes = ffxGetResourceVK(colorImg.image, colorFfxDesc,
            L"Static_FSR1_Color",  FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource outRes   = ffxGetResourceVK(outImg.image,   outFfxDesc,
            L"Static_FSR1_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        FfxFsr1DispatchDescription disp = {};
        disp.commandList      = ffxGetCommandListVK(cmd);
        disp.color            = colorRes;
        disp.output           = outRes;
        disp.renderSize       = { (uint32_t)inW, (uint32_t)inH };
        disp.enableSharpening = cfg.static_rcas;
        disp.sharpness        = cfg.static_sharpness;

        if (ffxFsr1ContextDispatch(ctx, &disp) != FFX_OK) {
            std::cout << "[StaticFSR1] FATAL: dispatch failed!\n";
            vkEndCommandBuffer(cmd);
            goto cleanup_fsr1_ctx;
        }

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    {
        std::vector<float> result;
        readbackImage(device, queue, cmd,
                      dlBuf, dlMem, outImg.image, outImg.layout,
                      outSize, (uint32_t)outW, (uint32_t)outH, result);
        saveFloatImage(outPath, outW, outH, result.data());
        std::cout << "[StaticFSR1] Saved -> " << outPath << "\n";
    }

cleanup_fsr1_ctx:
    ffxFsr1ContextDestroy(ctx);
cleanup_fsr1:
    _aligned_free(ctx);
    _aligned_free(scratch);
    freeTempImage(device, colorImg);
    freeTempImage(device, outImg);
    vkDestroyBuffer(device, uploadBuf, nullptr); vkFreeMemory(device, uploadMem, nullptr);
    vkDestroyBuffer(device, dlBuf,     nullptr); vkFreeMemory(device, dlMem,     nullptr);
    std::cout << "[StaticFSR1] Done.\n";
}

// ===========================================================================
//  FSR-2 static pass
// ===========================================================================

static void RunStaticFsr2(
    VkDevice device, VkPhysicalDevice physDev,
    VkQueue queue, VkCommandBuffer cmd,
    VkDeviceContext& vkDevCtx,
    const AppSettings& cfg,
    const std::vector<float>& inputLinear,
    int inW, int inH,
    int outW, int outH,
    const std::string& outPath)
{
    std::cout << "\n[StaticFSR2] -------- FSR 2.3.3 Static PNG Pass --------\n";
    std::cout << "[StaticFSR2] Input:  " << inW  << "x" << inH  << "\n";
    std::cout << "[StaticFSR2] Output: " << outW << "x" << outH << "\n";
    std::cout << "[StaticFSR2] Frames: " << cfg.static_jitter_frames
              << "  Jitter: " << (cfg.static_jitter ? "ON" : "OFF")
              << "  RCAS: "   << (cfg.static_rcas   ? "ON" : "OFF") << "\n";
    std::cout << "[StaticFSR2] Depth:  " << cfg.static_depth
              << "  (standard non-inverted, 0=near 1=far)\n";
    std::cout << "[StaticFSR2] AUTO_EXPOSURE: OFF (fixed preExposure=1.0)\n";

    VkDeviceSize colorUploadSize = (VkDeviceSize)(inW  * inH  * 4 * sizeof(float));
    VkDeviceSize depthUploadSize = (VkDeviceSize)(inW  * inH  * 1 * sizeof(float));
    VkDeviceSize mvUploadSize    = (VkDeviceSize)(inW  * inH  * 2 * sizeof(float));
    VkDeviceSize uploadSize      = colorUploadSize + depthUploadSize + mvUploadSize;
    VkDeviceSize outSize         = (VkDeviceSize)(outW * outH * 4 * sizeof(float));

    VkBuffer uploadBuf; VkDeviceMemory uploadMem;
    createBuffer(device, physDev, uploadSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 uploadBuf, uploadMem);
    VkBuffer dlBuf; VkDeviceMemory dlMem;
    createBuffer(device, physDev, outSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 dlBuf, dlMem);

    TempImage colorImg = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage depthImg = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage mvImg    = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage outImg   = makeTempImage(device, physDev, (uint32_t)outW, (uint32_t)outH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT      | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Note: no expImg — AUTO_EXPOSURE is disabled, we use fixed preExposure=1.0.

    std::vector<float> depthData(inW * inH, cfg.static_depth);
    std::vector<float> mvData   (inW * inH * 2, 0.0f);

    size_t scratchSize = ffxGetScratchMemorySizeVK(physDev, 4);
    void*  scratch     = _aligned_malloc(scratchSize, 64);
    memset(scratch, 0, scratchSize);

    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratch, scratchSize, 4);

    // -----------------------------------------------------------------------
    // Context flags for the static pass:
    //
    //  NO AUTO_EXPOSURE:
    //    FSR2's SPD luminance pyramid computes per-frame average luminance.
    //    For jittered static images, sub-pixel resampling causes tiny
    //    luminance differences between frames at high-contrast edges.
    //    FSR2 treats these as content changes → smear artifact.
    //    Fixed preExposure=1.0 avoids this entirely.
    //
    //  NO DEPTH_INVERTED:
    //    Static pass uses standard depth (0=near, 1=far).
    //
    //  MOTION_VECTORS_JITTER_CANCELLATION (when jitter is on):
    //    MVs carry the inter-frame jitter delta (prevJ - currJ).
    //    FSR2 subtracts the same delta, yielding true scene motion = 0.
    // -----------------------------------------------------------------------
    uint32_t fsr2Flags =
        FFX_FSR2_ENABLE_DEBUG_CHECKING     |
        FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    // Deliberately NO AUTO_EXPOSURE and NO DEPTH_INVERTED.

    bool useJitterCancel = false;
    if      (cfg.static_jitter_cancel == JitterCancel::ON)  useJitterCancel = true;
    else if (cfg.static_jitter_cancel == JitterCancel::OFF) useJitterCancel = false;
    else  /* APP_CONTROLLED */                              useJitterCancel = cfg.static_jitter;

    if (useJitterCancel)
        fsr2Flags |= FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    std::cout << "[StaticFSR2] JitterCancellationFlag="
              << (useJitterCancel ? "ON" : "OFF") << "\n";

    FfxFsr2ContextDescription fsr2Desc = {};
    fsr2Desc.flags            = fsr2Flags;
    fsr2Desc.maxRenderSize    = { (uint32_t)inW,  (uint32_t)inH  };
    fsr2Desc.displaySize      = { (uint32_t)outW, (uint32_t)outH };
    fsr2Desc.fpMessage        = StaticFfxMsg;
    fsr2Desc.backendInterface = ffxIface;

    FfxFsr2Context* ctx =
        (FfxFsr2Context*)_aligned_malloc(sizeof(FfxFsr2Context), 64);
    memset(ctx, 0, sizeof(FfxFsr2Context));
    if (ffxFsr2ContextCreate(ctx, &fsr2Desc) != FFX_OK) {
        std::cout << "[StaticFSR2] FATAL: ffxFsr2ContextCreate failed!\n";
        goto cleanup_fsr2;
    }
    std::cout << "[StaticFSR2] Context created.\n";

    // Pre-loop: transition output to GENERAL
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);
        transition(cmd, outImg.image, outImg.layout,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;

        FfxResourceDescription colorFfxDesc =
            ffxGetImageResourceDescriptionVK(colorImg.image, colorImg.info, FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription depthFfxDesc =
            ffxGetImageResourceDescriptionVK(depthImg.image, depthImg.info, FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription mvFfxDesc =
            ffxGetImageResourceDescriptionVK(mvImg.image,    mvImg.info,    FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription outFfxDesc =
            ffxGetImageResourceDescriptionVK(outImg.image,   outImg.info,   FFX_RESOURCE_USAGE_UAV);

        int32_t phaseCount = ffxFsr2GetJitterPhaseCount((int32_t)inW, (int32_t)outW);
        std::cout << "[StaticFSR2] Jitter phase count = " << phaseCount << "\n";

        ResampleMode resMode = ResampleMode::BILINEAR;
        switch (cfg.static_jitter_interp) {
        case JitterInterp::BICUBIC:  resMode = ResampleMode::BICUBIC;  break;
        case JitterInterp::LANCZOS3: resMode = ResampleMode::LANCZOS3; break;
        default:                     resMode = ResampleMode::BILINEAR; break;
        }

        std::vector<float> jitteredColor(inW * inH * 4);

        int32_t jitterIndex = 0;
        float   prevJX = 0.f, prevJY = 0.f;

        for (int frame = 0; frame < cfg.static_jitter_frames; frame++) {
            ++jitterIndex;
            float jX = 0.f, jY = 0.f;

            if (cfg.static_jitter) {
                ffxFsr2GetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

                ResampleWithShift(inputLinear.data(), inW, inH,
                                  jX, jY, jitteredColor.data(), resMode);

                // Inter-frame jitter delta in render-pixel space.
                // With JITTER_CANCELLATION, FSR2 subtracts the same delta
                // from MVs internally → true scene motion = 0.
                float mvX = prevJX - jX;
                float mvY = prevJY - jY;
                for (int p = 0; p < inW * inH; p++) {
                    mvData[p * 2 + 0] = mvX;
                    mvData[p * 2 + 1] = mvY;
                }
            } else {
                jX = 0.f; jY = 0.f;
                jitteredColor = inputLinear;
                std::fill(mvData.begin(), mvData.end(), 0.f);
            }

            void* mp;
            vkMapMemory(device, uploadMem, 0, uploadSize, 0, &mp);
            memcpy((uint8_t*)mp,                                     jitteredColor.data(), colorUploadSize);
            memcpy((uint8_t*)mp + colorUploadSize,                   depthData.data(),     depthUploadSize);
            memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, mvData.data(),        mvUploadSize);
            vkUnmapMemory(device, uploadMem);

            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &bi);

            transition(cmd, colorImg.image, colorImg.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, depthImg.image, depthImg.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, mvImg.image,    mvImg.layout,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy cR = {}; cR.bufferOffset = 0;
            cR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, colorImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR);

            VkBufferImageCopy dR = {}; dR.bufferOffset = colorUploadSize;
            dR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            dR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, depthImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR);

            VkBufferImageCopy mR = {}; mR.bufferOffset = colorUploadSize + depthUploadSize;
            mR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            mR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, mvImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR);

            transition(cmd, colorImg.image, colorImg.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, depthImg.image, depthImg.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, mvImg.image,    mvImg.layout,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            FfxResource colorRes = ffxGetResourceVK(colorImg.image, colorFfxDesc, L"Stc2_Color",  FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource depthRes = ffxGetResourceVK(depthImg.image, depthFfxDesc, L"Stc2_Depth",  FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource mvRes    = ffxGetResourceVK(mvImg.image,    mvFfxDesc,    L"Stc2_MV",     FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource outRes   = ffxGetResourceVK(outImg.image,   outFfxDesc,   L"Stc2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

            FfxFsr2DispatchDescription disp = {};
            disp.commandList         = ffxGetCommandListVK(cmd);
            disp.color               = colorRes;
            disp.depth               = depthRes;
            disp.motionVectors       = mvRes;
            disp.output              = outRes;
            disp.jitterOffset.x      = jX;
            disp.jitterOffset.y      = jY;
            disp.motionVectorScale.x = (float)inW;
            disp.motionVectorScale.y = (float)inH;
            disp.renderSize          = { (uint32_t)inW, (uint32_t)inH };
            disp.enableSharpening    = cfg.static_rcas;
            disp.sharpness           = cfg.static_sharpness;
            disp.frameTimeDelta      = 16.6f;
            // AUTO_EXPOSURE is OFF — preExposure=1.0 is used directly.
            disp.preExposure         = 1.f;
            disp.reset               = (frame == 0);
            disp.cameraNear          = 0.1f;
            disp.cameraFar           = 100.0f;
            disp.cameraFovAngleVertical  = 1.04719755f;
            disp.viewSpaceToMetersFactor = 1.f;

            if (ffxFsr2ContextDispatch(ctx, &disp) != FFX_OK) {
                std::cout << "[StaticFSR2] FATAL: dispatch failed frame "
                          << frame << "\n";
                vkEndCommandBuffer(cmd);
                goto cleanup_fsr2_ctx;
            }

            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            outImg.layout = VK_IMAGE_LAYOUT_GENERAL;

            if ((frame + 1) % 32 == 0 || frame == cfg.static_jitter_frames - 1)
                std::cout << "[StaticFSR2] Frame " << (frame + 1)
                          << "/" << cfg.static_jitter_frames << "\n";

            prevJX = jX;
            prevJY = jY;
        }
    }

    {
        std::vector<float> result;
        readbackImage(device, queue, cmd,
                      dlBuf, dlMem, outImg.image, outImg.layout,
                      outSize, (uint32_t)outW, (uint32_t)outH, result);
        saveFloatImage(outPath, outW, outH, result.data());
        std::cout << "[StaticFSR2] Saved -> " << outPath << "\n";
    }

cleanup_fsr2_ctx:
    ffxFsr2ContextDestroy(ctx);
cleanup_fsr2:
    _aligned_free(ctx);
    _aligned_free(scratch);
    freeTempImage(device, colorImg);
    freeTempImage(device, depthImg);
    freeTempImage(device, mvImg);
    freeTempImage(device, outImg);
    vkDestroyBuffer(device, uploadBuf, nullptr); vkFreeMemory(device, uploadMem, nullptr);
    vkDestroyBuffer(device, dlBuf,     nullptr); vkFreeMemory(device, dlMem,     nullptr);
    std::cout << "[StaticFSR2] Done.\n";
}

// ===========================================================================
//  FSR-3 static pass
// ===========================================================================

static void RunStaticFsr3(
    VkDevice device, VkPhysicalDevice physDev,
    VkQueue queue, VkCommandBuffer cmd,
    VkDeviceContext& vkDevCtx,
    const AppSettings& cfg,
    const std::vector<float>& inputLinear,
    int inW, int inH,
    int outW, int outH,
    const std::string& outPath)
{
    std::cout << "\n[StaticFSR3] -------- FSR 3.1.4 Static PNG Pass --------\n";
    std::cout << "[StaticFSR3] Input:  " << inW  << "x" << inH  << "\n";
    std::cout << "[StaticFSR3] Output: " << outW << "x" << outH << "\n";
    std::cout << "[StaticFSR3] Frames: " << cfg.static_jitter_frames
              << "  Jitter: " << (cfg.static_jitter ? "ON" : "OFF")
              << "  RCAS: "   << (cfg.static_rcas   ? "ON" : "OFF") << "\n";
    std::cout << "[StaticFSR3] Depth:  " << cfg.static_depth
              << "  (standard non-inverted, 0=near 1=far)\n";
    std::cout << "[StaticFSR3] AUTO_EXPOSURE: OFF (fixed preExposure=1.0)\n";

    VkDeviceSize colorUploadSize = (VkDeviceSize)(inW * inH * 4 * sizeof(float));
    VkDeviceSize depthUploadSize = (VkDeviceSize)(inW * inH * 1 * sizeof(float));
    VkDeviceSize mvUploadSize    = (VkDeviceSize)(inW * inH * 2 * sizeof(float));
    VkDeviceSize uploadSize      = colorUploadSize + depthUploadSize + mvUploadSize;
    VkDeviceSize outSize         = (VkDeviceSize)(outW * outH * 4 * sizeof(float));

    VkBuffer uploadBuf; VkDeviceMemory uploadMem;
    createBuffer(device, physDev, uploadSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 uploadBuf, uploadMem);
    VkBuffer dlBuf; VkDeviceMemory dlMem;
    createBuffer(device, physDev, outSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 dlBuf, dlMem);

    TempImage colorImg = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage depthImg = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage mvImg    = makeTempImage(device, physDev, (uint32_t)inW, (uint32_t)inH,
        VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TempImage outImg   = makeTempImage(device, physDev, (uint32_t)outW, (uint32_t)outH,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT      | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Note: no expImg — AUTO_EXPOSURE is disabled.

    std::vector<float> depthData(inW * inH, cfg.static_depth);
    std::vector<float> mvData   (inW * inH * 2, 0.0f);

    size_t scratchSize = ffxGetScratchMemorySizeVK(physDev, 4);
    void*  scratch     = _aligned_malloc(scratchSize, 64);
    memset(scratch, 0, scratchSize);

    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratch, scratchSize, 4);

    // -----------------------------------------------------------------------
    // Context flags for the static pass:
    //
    //  NO AUTO_EXPOSURE: same reason as FSR2 — avoids smear artifact.
    //
    //  NO DEPTH_INVERTED, NO DEPTH_INFINITE:
    //    Standard depth (0=near, 1=far), finite far plane at 100.0.
    //
    //  MOTION_VECTORS_JITTER_CANCELLATION (when jitter is on):
    //    Same encoding as FSR2: MVs carry inter-frame jitter delta.
    // -----------------------------------------------------------------------
    uint32_t fsr3Flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    // Deliberately NO AUTO_EXPOSURE, NO DEPTH_INVERTED, NO DEPTH_INFINITE.

    bool useJitterCancel = false;
    if      (cfg.static_jitter_cancel == JitterCancel::ON)  useJitterCancel = true;
    else if (cfg.static_jitter_cancel == JitterCancel::OFF) useJitterCancel = false;
    else  /* APP_CONTROLLED */                              useJitterCancel = cfg.static_jitter;

    if (useJitterCancel)
        fsr3Flags |= FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    std::cout << "[StaticFSR3] JitterCancellationFlag="
              << (useJitterCancel ? "ON" : "OFF") << "\n";

    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    fsr3Desc.flags            = fsr3Flags;
    fsr3Desc.maxRenderSize    = { (uint32_t)inW,  (uint32_t)inH  };
    fsr3Desc.maxUpscaleSize   = { (uint32_t)outW, (uint32_t)outH };
    fsr3Desc.fpMessage        = StaticFfxMsg;
    fsr3Desc.backendInterface = ffxIface;

    FfxFsr3UpscalerContext* ctx =
        (FfxFsr3UpscalerContext*)_aligned_malloc(sizeof(FfxFsr3UpscalerContext), 64);
    memset(ctx, 0, sizeof(FfxFsr3UpscalerContext));
    if (ffxFsr3UpscalerContextCreate(ctx, &fsr3Desc) != FFX_OK) {
        std::cout << "[StaticFSR3] FATAL: ffxFsr3UpscalerContextCreate failed!\n";
        goto cleanup_fsr3;
    }
    std::cout << "[StaticFSR3] Context created.\n";

    {
        FfxFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
        if (ffxFsr3UpscalerGetSharedResourceDescriptions(ctx, &sharedDescs) != FFX_OK) {
            std::cout << "[StaticFSR3] FATAL: GetSharedResourceDescriptions failed!\n";
            goto cleanup_fsr3_ctx;
        }

        SharedImage siRecon    = createSharedImage(device, physDev, sharedDescs.reconstructedPrevNearestDepth);
        SharedImage siDilDepth = createSharedImage(device, physDev, sharedDescs.dilatedDepth);
        SharedImage siDilMV    = createSharedImage(device, physDev, sharedDescs.dilatedMotionVectors);

        // -----------------------------------------------------------------------
        // Sentinel for reconstructedPrevNearestDepth — standard (non-inverted):
        //   Near geometry has small float depth → small uint (asuint).
        //   FSR3 uses InterlockedMin to find nearest.
        //   Sentinel = 0xFFFFFFFF (maximum uint) so any real depth value wins.
        //
        // Sentinel for dilatedDepth and dilatedMotionVectors:
        //   These are output-only UAVs written by the PrepareInputs pass each
        //   frame.  However, if PrepareInputs reads them before writing
        //   (e.g. border tiles), undefined memory causes the top-right
        //   rectangular shadow artifact.  Clear both to zero before frame 0.
        //   Zero depth = "near / no geometry" which is a safe default.
        //   Zero MV = no motion, also safe.
        // -----------------------------------------------------------------------
        VkClearColorValue reconSentinel = {};
        reconSentinel.uint32[0] = 0xFFFFFFFFu;
        reconSentinel.uint32[1] = 0xFFFFFFFFu;
        reconSentinel.uint32[2] = 0xFFFFFFFFu;
        reconSentinel.uint32[3] = 0xFFFFFFFFu;

        VkClearColorValue zeroClear = {};
        zeroClear.float32[0] = 0.f;
        zeroClear.float32[1] = 0.f;
        zeroClear.float32[2] = 0.f;
        zeroClear.float32[3] = 0.f;

        VkImageSubresourceRange reconRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, siRecon.info.mipLevels,    0, 1 };
        VkImageSubresourceRange dilDepthRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, siDilDepth.info.mipLevels, 0, 1 };
        VkImageSubresourceRange dilMVRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, siDilMV.info.mipLevels,    0, 1 };

        // Pre-loop: clear all shared resources and transition output to GENERAL.
        {
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &bi);

            // reconstructedPrevNearestDepth — sentinel (max uint for InterlockedMin)
            transition(cmd, siRecon.image, siRecon.layout,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);
            vkCmdClearColorImage(cmd, siRecon.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &reconSentinel, 1, &reconRange);
            transition(cmd, siRecon.image, siRecon.layout,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);

            // dilatedDepth — zero (safe "no geometry" value)
            transition(cmd, siDilDepth.image, siDilDepth.layout,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siDilDepth.info.mipLevels);
            vkCmdClearColorImage(cmd, siDilDepth.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zeroClear, 1, &dilDepthRange);
            transition(cmd, siDilDepth.image, siDilDepth.layout,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siDilDepth.info.mipLevels);

            // dilatedMotionVectors — zero (no motion)
            transition(cmd, siDilMV.image, siDilMV.layout,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siDilMV.info.mipLevels);
            vkCmdClearColorImage(cmd, siDilMV.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zeroClear, 1, &dilMVRange);
            transition(cmd, siDilMV.image, siDilMV.layout,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siDilMV.info.mipLevels);

            // output image
            transition(cmd, outImg.image, outImg.layout,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);
        }

        FfxResourceDescription colorFfxDesc =
            ffxGetImageResourceDescriptionVK(colorImg.image, colorImg.info, FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription depthFfxDesc =
            ffxGetImageResourceDescriptionVK(depthImg.image, depthImg.info, FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription mvFfxDesc =
            ffxGetImageResourceDescriptionVK(mvImg.image,    mvImg.info,    FFX_RESOURCE_USAGE_READ_ONLY);
        FfxResourceDescription outFfxDesc =
            ffxGetImageResourceDescriptionVK(outImg.image,   outImg.info,   FFX_RESOURCE_USAGE_UAV);
        FfxResourceDescription reconFfxDesc =
            ffxGetImageResourceDescriptionVK(siRecon.image,    siRecon.info,    FFX_RESOURCE_USAGE_UAV);
        FfxResourceDescription dilDepthFfxDesc =
            ffxGetImageResourceDescriptionVK(siDilDepth.image, siDilDepth.info, FFX_RESOURCE_USAGE_UAV);
        FfxResourceDescription dilMVFfxDesc =
            ffxGetImageResourceDescriptionVK(siDilMV.image,    siDilMV.info,    FFX_RESOURCE_USAGE_UAV);

        int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount((int32_t)inW, (int32_t)outW);
        std::cout << "[StaticFSR3] Jitter phase count = " << phaseCount << "\n";

        ResampleMode resMode = ResampleMode::BILINEAR;
        switch (cfg.static_jitter_interp) {
        case JitterInterp::BICUBIC:  resMode = ResampleMode::BICUBIC;  break;
        case JitterInterp::LANCZOS3: resMode = ResampleMode::LANCZOS3; break;
        default:                     resMode = ResampleMode::BILINEAR; break;
        }

        std::vector<float> jitteredColor(inW * inH * 4);

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;

        int32_t jitterIndex = 0;
        float   prevJX = 0.f, prevJY = 0.f;

        for (int frame = 0; frame < cfg.static_jitter_frames; frame++) {
            ++jitterIndex;
            float jX = 0.f, jY = 0.f;

            if (cfg.static_jitter) {
                ffxFsr3UpscalerGetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

                ResampleWithShift(inputLinear.data(), inW, inH,
                                  jX, jY, jitteredColor.data(), resMode);

                float mvX = prevJX - jX;
                float mvY = prevJY - jY;
                for (int p = 0; p < inW * inH; p++) {
                    mvData[p * 2 + 0] = mvX;
                    mvData[p * 2 + 1] = mvY;
                }
            } else {
                jX = 0.f; jY = 0.f;
                jitteredColor = inputLinear;
                std::fill(mvData.begin(), mvData.end(), 0.f);
            }

            void* mp;
            vkMapMemory(device, uploadMem, 0, uploadSize, 0, &mp);
            memcpy((uint8_t*)mp,                                     jitteredColor.data(), colorUploadSize);
            memcpy((uint8_t*)mp + colorUploadSize,                   depthData.data(),     depthUploadSize);
            memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, mvData.data(),        mvUploadSize);
            vkUnmapMemory(device, uploadMem);

            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &bi);

            // Clear reconSentinel every frame (FSR3 writes to it via
            // InterlockedMin; last frame's values must not persist).
            transition(cmd, siRecon.image, siRecon.layout,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);
            vkCmdClearColorImage(cmd, siRecon.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &reconSentinel, 1, &reconRange);
            transition(cmd, siRecon.image, siRecon.layout,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);

            transition(cmd, colorImg.image, colorImg.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, depthImg.image, depthImg.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, mvImg.image,    mvImg.layout,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy cR = {}; cR.bufferOffset = 0;
            cR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, colorImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR);

            VkBufferImageCopy dR = {}; dR.bufferOffset = colorUploadSize;
            dR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            dR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, depthImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR);

            VkBufferImageCopy mR = {}; mR.bufferOffset = colorUploadSize + depthUploadSize;
            mR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            mR.imageExtent = { (uint32_t)inW, (uint32_t)inH, 1 };
            vkCmdCopyBufferToImage(cmd, uploadBuf, mvImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR);

            transition(cmd, colorImg.image, colorImg.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, depthImg.image, depthImg.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            transition(cmd, mvImg.image,    mvImg.layout,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            FfxResource colorRes = ffxGetResourceVK(colorImg.image,   colorFfxDesc,    L"Stc3_Color",    FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource depthRes = ffxGetResourceVK(depthImg.image,   depthFfxDesc,    L"Stc3_Depth",    FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource mvRes    = ffxGetResourceVK(mvImg.image,      mvFfxDesc,       L"Stc3_MV",       FFX_RESOURCE_STATE_COMPUTE_READ);
            FfxResource outRes   = ffxGetResourceVK(outImg.image,     outFfxDesc,      L"Stc3_Output",   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            FfxResource reconRes = ffxGetResourceVK(siRecon.image,    reconFfxDesc,    L"Stc3_Recon",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            FfxResource dilDRes  = ffxGetResourceVK(siDilDepth.image, dilDepthFfxDesc, L"Stc3_DilDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            FfxResource dilMRes  = ffxGetResourceVK(siDilMV.image,    dilMVFfxDesc,    L"Stc3_DilMV",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);

            FfxFsr3UpscalerDispatchDescription disp = {};
            disp.commandList   = ffxGetCommandListVK(cmd);
            disp.color         = colorRes;
            disp.depth         = depthRes;
            disp.motionVectors = mvRes;
            disp.output        = outRes;
            disp.reconstructedPrevNearestDepth = reconRes;
            disp.dilatedDepth                  = dilDRes;
            disp.dilatedMotionVectors          = dilMRes;

            disp.jitterOffset.x          = jX;
            disp.jitterOffset.y          = jY;
            disp.motionVectorScale.x     = (float)inW;
            disp.motionVectorScale.y     = (float)inH;
            disp.renderSize              = { (uint32_t)inW,  (uint32_t)inH  };
            disp.upscaleSize             = { (uint32_t)outW, (uint32_t)outH };
            disp.enableSharpening        = cfg.static_rcas;
            disp.sharpness               = cfg.static_sharpness;
            disp.frameTimeDelta          = 16.6f;
            // AUTO_EXPOSURE is OFF — preExposure=1.0 is used directly.
            disp.preExposure             = 1.f;
            disp.reset                   = (frame == 0);
            disp.cameraNear              = 0.1f;
            disp.cameraFar               = 100.0f;
            disp.cameraFovAngleVertical  = 1.04719755f;
            disp.viewSpaceToMetersFactor = 1.f;
            disp.flags                   = 0;

            if (ffxFsr3UpscalerContextDispatch(ctx, &disp) != FFX_OK) {
                std::cout << "[StaticFSR3] FATAL: dispatch failed frame "
                          << frame << "\n";
                vkEndCommandBuffer(cmd);
                vkDestroyImage(device, siRecon.image,     nullptr); vkFreeMemory(device, siRecon.memory,    nullptr);
                vkDestroyImage(device, siDilDepth.image,  nullptr); vkFreeMemory(device, siDilDepth.memory, nullptr);
                vkDestroyImage(device, siDilMV.image,     nullptr); vkFreeMemory(device, siDilMV.memory,    nullptr);
                goto cleanup_fsr3_ctx;
            }

            VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT  | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &memBarrier, 0, nullptr, 0, nullptr);

            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            outImg.layout = VK_IMAGE_LAYOUT_GENERAL;

            if ((frame + 1) % 32 == 0 || frame == cfg.static_jitter_frames - 1)
                std::cout << "[StaticFSR3] Frame " << (frame + 1)
                          << "/" << cfg.static_jitter_frames << "\n";

            prevJX = jX;
            prevJY = jY;
        }

        {
            std::vector<float> result;
            readbackImage(device, queue, cmd,
                          dlBuf, dlMem, outImg.image, outImg.layout,
                          outSize, (uint32_t)outW, (uint32_t)outH, result);
            saveFloatImage(outPath, outW, outH, result.data());
            std::cout << "[StaticFSR3] Saved -> " << outPath << "\n";
        }

        vkDestroyImage(device, siRecon.image,     nullptr); vkFreeMemory(device, siRecon.memory,    nullptr);
        vkDestroyImage(device, siDilDepth.image,  nullptr); vkFreeMemory(device, siDilDepth.memory, nullptr);
        vkDestroyImage(device, siDilMV.image,     nullptr); vkFreeMemory(device, siDilMV.memory,    nullptr);
    }

cleanup_fsr3_ctx:
    ffxFsr3UpscalerContextDestroy(ctx);
cleanup_fsr3:
    _aligned_free(ctx);
    _aligned_free(scratch);
    freeTempImage(device, colorImg);
    freeTempImage(device, depthImg);
    freeTempImage(device, mvImg);
    freeTempImage(device, outImg);
    vkDestroyBuffer(device, uploadBuf, nullptr); vkFreeMemory(device, uploadMem, nullptr);
    vkDestroyBuffer(device, dlBuf,     nullptr); vkFreeMemory(device, dlMem,     nullptr);
    std::cout << "[StaticFSR3] Done.\n";
}

// ===========================================================================
//  Public entry point
// ===========================================================================

void RunStaticPngPass(
    VkDevice            device,
    VkPhysicalDevice    physicalDevice,
    VkQueue             queue,
    VkCommandBuffer     cmd,
    VkDeviceContext&    vkDevCtx,
    const AppSettings&  cfg)
{
    std::cout << "\n============================================\n";
    std::cout << "  Static PNG Operations Pass\n";
    std::cout << "============================================\n";

    int inW = 0, inH = 0, inChannels = 0;
    stbi_uc* rawPixels = stbi_load(cfg.static_input_name.c_str(),
                                   &inW, &inH, &inChannels, 4);
    if (!rawPixels) {
        std::cout << "[StaticPass] ERROR: Cannot load '"
                  << cfg.static_input_name << "': "
                  << stbi_failure_reason() << "\n";
        return;
    }
    std::cout << "[StaticPass] Loaded '" << cfg.static_input_name
              << "' -> " << inW << "x" << inH
              << "  channels=" << inChannels << "\n";

    // Convert uint8 RGBA → float linear RGBA (sRGB gamma 2.2 decode)
    const int npixels = inW * inH;
    std::vector<float> inputLinear(npixels * 4);
    for (int i = 0; i < npixels * 4; i++) {
        float srgb = rawPixels[i] / 255.0f;
        inputLinear[i] = powf(srgb, 2.2f);
    }
    stbi_image_free(rawPixels);

    int outW, outH;
    if (cfg.static_upscaling && cfg.static_scale > 1.0f) {
        outW = (int)std::round((float)inW * cfg.static_scale);
        outH = (int)std::round((float)inH * cfg.static_scale);
        outW = (outW + 1) & ~1;
        outH = (outH + 1) & ~1;
    } else {
        outW = inW;
        outH = inH;
    }
    std::cout << "[StaticPass] Render resolution : " << inW  << "x" << inH  << "\n";
    std::cout << "[StaticPass] Display resolution: " << outW << "x" << outH << "\n";

    std::string algoStr = (cfg.static_algo == StaticAlgorithm::FSR1) ? "FSR1" :
                          (cfg.static_algo == StaticAlgorithm::FSR2) ? "FSR2" : "FSR3";
    std::string outName = cfg.static_output_name;
    {
        size_t dot = outName.rfind('.');
        std::string suffix = "_" + algoStr +
                             "_scale" + std::to_string((int)(cfg.static_scale * 10)) +
                             "x10" +
                             (cfg.static_rcas ? "_rcas" : "") +
                             (cfg.static_jitter && cfg.static_algo != StaticAlgorithm::FSR1
                              ? "_" + std::to_string(cfg.static_jitter_frames) + "f" : "");
        if (dot != std::string::npos)
            outName.insert(dot, suffix);
        else
            outName += suffix;
    }
    std::cout << "[StaticPass] Output file: " << outName << "\n";

    switch (cfg.static_algo) {
    case StaticAlgorithm::FSR1:
        RunStaticFsr1(device, physicalDevice, queue, cmd, vkDevCtx,
                      cfg, inputLinear, inW, inH, outW, outH, outName);
        break;
    case StaticAlgorithm::FSR2:
        RunStaticFsr2(device, physicalDevice, queue, cmd, vkDevCtx,
                      cfg, inputLinear, inW, inH, outW, outH, outName);
        break;
    case StaticAlgorithm::FSR3:
        RunStaticFsr3(device, physicalDevice, queue, cmd, vkDevCtx,
                      cfg, inputLinear, inW, inH, outW, outH, outName);
        break;
    }

    std::cout << "[StaticPass] Complete.\n";
}

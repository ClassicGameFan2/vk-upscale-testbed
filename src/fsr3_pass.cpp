#include "common.h"

static void Fsr3MsgCallback(FfxMsgType /*type*/, const wchar_t* message) {
    if (!message) return;
    std::cout << "[FSR3 MSG] ";
    for (int i = 0; i < 2048 && message[i]; ++i)
        std::cout << (char)message[i];
    std::cout << "\n";
    std::cout.flush();
}

// ---------------------------------------------------------------------------
// Helper: clear an image to zero using a transfer clear, then leave it in
// GENERAL (UAV) layout.  Used to initialise shared resources so that frame 0
// sees zeroed data even when reset=true is set.
// ---------------------------------------------------------------------------
static void clearImageToZero(VkCommandBuffer cmd,
                              VkImage image, VkImageLayout& layout,
                              uint32_t mipLevels)
{
    VkImageSubresourceRange range = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };

    transition(cmd, image, layout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    VkClearColorValue clearVal = {};   // all zeros
    vkCmdClearColorImage(cmd, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearVal, 1, &range);

    transition(cmd, image, layout,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
}

void RunFsr3Pass(
    VkDevice device, VkPhysicalDevice physicalDevice,
    VkQueue queue, VkCommandBuffer cmd,
    VkBuffer uploadBuffer, VkDeviceMemory uploadMemory,
    VkBuffer downloadBuffer, VkDeviceMemory downloadMemory,
    VkImage colorImage,  VkImageCreateInfo colorInfo,
    VkImage depthImage,  VkImageCreateInfo depthInfo,
    VkImage mvImage,     VkImageCreateInfo mvInfo,
    VkImage outputImage, VkImageCreateInfo outputInfo,
    VkDeviceSize colorUploadSize, VkDeviceSize depthUploadSize,
    VkDeviceSize mvUploadSize,    VkDeviceSize outSize,
    VkDeviceContext& vkDevCtx,
    void* scratchBuffer, size_t scratchBufferSize)
{
    std::cout << "\n============================================\n";
    std::cout << "  FSR 3.1.4 Temporal Upscaling Test (128 frames)\n";
    std::cout << "============================================\n";

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    memset(scratchBuffer, 0, scratchBufferSize);

    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratchBuffer, scratchBufferSize, 4);

    // ── Context creation ────────────────────────────────────────────────────
    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    fsr3Desc.flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE                 |
        FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE                      |
        FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
        FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED                     |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    fsr3Desc.maxRenderSize  = { RENDER_W,  RENDER_H  };
    fsr3Desc.maxUpscaleSize = { DISPLAY_W, DISPLAY_H };
    fsr3Desc.fpMessage      = Fsr3MsgCallback;
    fsr3Desc.backendInterface = ffxIface;

    FfxFsr3UpscalerContext* ctx =
        (FfxFsr3UpscalerContext*)_aligned_malloc(
            sizeof(FfxFsr3UpscalerContext), 64);
    memset(ctx, 0, sizeof(FfxFsr3UpscalerContext));

    if (ffxFsr3UpscalerContextCreate(ctx, &fsr3Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr3UpscalerContextCreate failed!\n";
        _aligned_free(ctx);
        return;
    }
    std::cout << "[FSR3] Context created OK.\n";

    // ── Shared resource descriptions ────────────────────────────────────────
    FfxFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(ctx, &sharedDescs) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr3UpscalerGetSharedResourceDescriptions failed!\n";
        ffxFsr3UpscalerContextDestroy(ctx);
        _aligned_free(ctx);
        return;
    }

    std::cout << "[FSR3] Shared resource descriptions:\n";
    std::cout << "  reconstructedPrevNearestDepth: "
              << sharedDescs.reconstructedPrevNearestDepth.resourceDescription.width
              << "x"
              << sharedDescs.reconstructedPrevNearestDepth.resourceDescription.height
              << " fmt=" << (int)sharedDescs.reconstructedPrevNearestDepth.resourceDescription.format
              << "\n";
    std::cout << "  dilatedDepth: "
              << sharedDescs.dilatedDepth.resourceDescription.width
              << "x"
              << sharedDescs.dilatedDepth.resourceDescription.height
              << " fmt=" << (int)sharedDescs.dilatedDepth.resourceDescription.format
              << "\n";
    std::cout << "  dilatedMotionVectors: "
              << sharedDescs.dilatedMotionVectors.resourceDescription.width
              << "x"
              << sharedDescs.dilatedMotionVectors.resourceDescription.height
              << " fmt=" << (int)sharedDescs.dilatedMotionVectors.resourceDescription.format
              << "\n";
    std::cout.flush();

    SharedImage siRecon    = createSharedImage(device, physicalDevice,
                                               sharedDescs.reconstructedPrevNearestDepth);
    SharedImage siDilDepth = createSharedImage(device, physicalDevice,
                                               sharedDescs.dilatedDepth);
    SharedImage siDilMV    = createSharedImage(device, physicalDevice,
                                               sharedDescs.dilatedMotionVectors);
    std::cout << "[FSR3] Shared images allocated.\n";

    // ── Pre-loop: clear shared resources to zero and bring output to GENERAL ─
    // Clearing to zero prevents garbage disocclusion detection artifacts on
    // the very first frame (before reset=true has had a chance to initialise
    // the internal history).  The shared resources are left in GENERAL (UAV)
    // layout, which is how we declare them to the FSR3 backend each frame.
    {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        clearImageToZero(cmd, siRecon.image,    siRecon.layout,
                         siRecon.info.mipLevels);
        clearImageToZero(cmd, siDilDepth.image, siDilDepth.layout,
                         siDilDepth.info.mipLevels);
        clearImageToZero(cmd, siDilMV.image,    siDilMV.layout,
                         siDilMV.info.mipLevels);

        // Bring outputImage to GENERAL as well.
        VkImageLayout outLay = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, outputImage, outLay,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    // ── Per-frame resource descriptions (built once, reused every frame) ────
    FfxResourceDescription colorDesc =
        ffxGetImageResourceDescriptionVK(colorImage,  colorInfo,
                                         FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc =
        ffxGetImageResourceDescriptionVK(depthImage,  depthInfo,
                                         FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription mvDesc =
        ffxGetImageResourceDescriptionVK(mvImage,     mvInfo,
                                         FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc =
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo,
                                         FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription reconDesc =
        ffxGetImageResourceDescriptionVK(siRecon.image,    siRecon.info,
                                         FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilDepthDesc =
        ffxGetImageResourceDescriptionVK(siDilDepth.image, siDilDepth.info,
                                         FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilMVDesc =
        ffxGetImageResourceDescriptionVK(siDilMV.image,    siDilMV.info,
                                         FFX_RESOURCE_USAGE_UAV);

    // ── Jitter setup ────────────────────────────────────────────────────────
    int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(RENDER_W, DISPLAY_W);
    std::cout << "[FSR3] Jitter phase count: " << phaseCount << "\n";

    const int totalFrames = 128;
    std::cout << "[FSR3] Running " << totalFrames << " frames...\n";
    std::cout.flush();

    VkDeviceSize uploadSize = colorUploadSize + depthUploadSize + mvUploadSize;

    // Layout tracking for the input images and the output image.
    // After the pre-loop command above, outputImage is in GENERAL.
    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Shared images are tracked inside SharedImage::layout (already GENERAL).

    int32_t jitterIndex = 0;
    float   prevJX = 0.f, prevJY = 0.f;
    void*   outData = nullptr;

    for (int i = 0; i < totalFrames; i++) {
        ++jitterIndex;
        float jX = 0.f, jY = 0.f;
        ffxFsr3UpscalerGetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

        // ── CPU render ──────────────────────────────────────────────────────
        std::vector<float> fColor(RENDER_W * RENDER_H * 4);
        std::vector<float> fDepth(RENDER_W * RENDER_H);
        std::vector<float> fMV   (RENDER_W * RENDER_H * 2);
        renderScene(RENDER_W, RENDER_H, jX, jY, prevJX, prevJY,
                    fColor.data(), fDepth.data(), fMV.data());

        // ── Upload ──────────────────────────────────────────────────────────
        void* mp;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mp);
        memcpy((uint8_t*)mp,                                  fColor.data(), colorUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize,                fDepth.data(), depthUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, fMV.data(), mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        // ── Build command buffer ─────────────────────────────────────────────
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Transfer inputs into their images
        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage,    mvLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cR = {}; cR.bufferOffset = 0;
        cR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR);

        VkBufferImageCopy dR = {}; dR.bufferOffset = colorUploadSize;
        dR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        dR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR);

        VkBufferImageCopy mR = {}; mR.bufferOffset = colorUploadSize + depthUploadSize;
        mR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        mR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, mvImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR);

        // Transition inputs to shader-read
        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage,    mvLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // ── Build FfxResource wrappers ───────────────────────────────────────
        // Input images are in SHADER_READ_ONLY → COMPUTE_READ state.
        // Output and shared images are in GENERAL → UNORDERED_ACCESS state.
        // We declare the *current* state so the backend emits correct barriers.
        FfxResource colorRes = ffxGetResourceVK(colorImage,  colorDesc,
            L"FSR3_Color",  FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource depthRes = ffxGetResourceVK(depthImage,  depthDesc,
            L"FSR3_Depth",  FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource mvRes    = ffxGetResourceVK(mvImage,     mvDesc,
            L"FSR3_MVs",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource outRes   = ffxGetResourceVK(outputImage, outDesc,
            L"FSR3_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource reconRes = ffxGetResourceVK(siRecon.image,    reconDesc,
            L"FSR3_Recon",  FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilDepthRes = ffxGetResourceVK(siDilDepth.image, dilDepthDesc,
            L"FSR3_DilDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilMVRes = ffxGetResourceVK(siDilMV.image,    dilMVDesc,
            L"FSR3_DilMV",  FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        // ── Dispatch ────────────────────────────────────────────────────────
        FfxFsr3UpscalerDispatchDescription disp = {};
        disp.commandList   = ffxGetCommandListVK(cmd);
        disp.color         = colorRes;
        disp.depth         = depthRes;
        disp.motionVectors = mvRes;
        disp.output        = outRes;

        disp.reconstructedPrevNearestDepth = reconRes;
        disp.dilatedDepth                  = dilDepthRes;
        disp.dilatedMotionVectors          = dilMVRes;

        // Jitter: FSR3 uses the same convention as FSR2 in our setup.
        disp.jitterOffset.x = jX;
        disp.jitterOffset.y = jY;

        // Motion vector scale: same as FSR2 - vectors are in render-pixel space.
        disp.motionVectorScale.x = (float)RENDER_W;
        disp.motionVectorScale.y = (float)RENDER_H;

        disp.renderSize  = { RENDER_W,  RENDER_H  };
        disp.upscaleSize = { DISPLAY_W, DISPLAY_H };

        disp.enableSharpening = true;
        disp.sharpness        = 0.8f;
        disp.frameTimeDelta   = 16.6f;
        disp.preExposure      = 1.f;
        disp.reset            = (i == 0);

        // Inverted depth: near plane is FLT_MAX, far is 0.1
        disp.cameraNear             = FLT_MAX;
        disp.cameraFar              = 0.1f;
        disp.cameraFovAngleVertical = 1.04719755f;
        disp.viewSpaceToMetersFactor = 1.f;

        disp.flags = 0;

        if (ffxFsr3UpscalerContextDispatch(ctx, &disp) != FFX_OK) {
            std::cout << "[FATAL] FSR3 dispatch failed frame " << i << "\n";
            break;
        }

        // ── Full memory barrier after dispatch ───────────────────────────────
        // Ensures all compute writes from the FSR3 passes are visible before
        // we attempt any readback or the next frame's upload.
        VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                   VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT  |
                                   VK_ACCESS_MEMORY_READ_BIT  |
                                   VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &memBarrier, 0, nullptr, 0, nullptr);

        vkEndCommandBuffer(cmd);

        VkResult sr = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueSubmit failed frame " << i
                      << " result=" << sr << "\n";
            break;
        }
        VkResult wr = vkQueueWaitIdle(queue);
        if (wr != VK_SUCCESS) {
            std::cout << "[FATAL] vkQueueWaitIdle failed frame " << i
                      << " result=" << wr << "\n";
            break;
        }

        // After the FSR3 backend finishes, the outputImage is in GENERAL.
        // Keep our tracking in sync.
        outputLayout = VK_IMAGE_LAYOUT_GENERAL;

        // ── Snapshot readback ────────────────────────────────────────────────
        int frameNum = i + 1;
        if (frameNum == 32 || frameNum == 64 ||
            frameNum == 96 || frameNum == 128)
        {
            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &beginInfo);

            transition(cmd, outputImage, outputLayout,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy or2 = {};
            or2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            or2.imageExtent = { DISPLAY_W, DISPLAY_H, 1 };
            vkCmdCopyImageToBuffer(cmd, outputImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   downloadBuffer, 1, &or2);

            // Return to GENERAL for next dispatch.
            transition(cmd, outputImage, outputLayout,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
            std::string fname = "FSR_3.1.4_frame" + std::to_string(frameNum) + ".png";
            saveFloatImage(fname, DISPLAY_W, DISPLAY_H, (float*)outData);
            vkUnmapMemory(device, downloadMemory);
            std::cout << "[FSR3] Snapshot frame " << frameNum << "\n";
        }

        if (frameNum % 32 == 0)
            std::cout << "[FSR3] Frame " << frameNum << "/" << totalFrames << "\n";

        prevJX = jX;
        prevJY = jY;
    }

    // ── Final readback ────────────────────────────────────────────────────
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);
    transition(cmd, outputImage, outputLayout,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy or3 = {};
    or3.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    or3.imageExtent = { DISPLAY_W, DISPLAY_H, 1 };
    vkCmdCopyImageToBuffer(cmd, outputImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           downloadBuffer, 1, &or3);
    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_3.1.4_2x.png", DISPLAY_W, DISPLAY_H, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    // ── Cleanup ──────────────────────────────────────────────────────────
    ffxFsr3UpscalerContextDestroy(ctx);
    _aligned_free(ctx);

    vkDestroyImage(device, siRecon.image,    nullptr);
    vkFreeMemory  (device, siRecon.memory,   nullptr);
    vkDestroyImage(device, siDilDepth.image, nullptr);
    vkFreeMemory  (device, siDilDepth.memory, nullptr);
    vkDestroyImage(device, siDilMV.image,    nullptr);
    vkFreeMemory  (device, siDilMV.memory,   nullptr);

    std::cout << "[FSR3] Done.\n";
}

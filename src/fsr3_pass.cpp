#include "common.h"

static void Fsr3MsgCallback(FfxMsgType /*type*/, const wchar_t* message) {
    if (!message) return;
    std::cout << "[FSR3 MSG] ";
    for (int i = 0; i < 2048 && message[i]; ++i)
        std::cout << (char)message[i];
    std::cout << "\n";
    std::cout.flush();
}

// =============================================================================
//  RunFsr3Sequence
//
//  Faithfully implements the Cauldron / AMD SDK FSR 3.1.4 upscaler loop.
//
//  Key facts derived from SDK documentation:
//
//  1. Context flags:
//     - FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE : input is linear HDR
//     - FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE       : FSR computes exposure
//     - FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED      : depth = zNear/z (reversed)
//     - FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING      : validate API usage
//     NOTE: we do NOT set ENABLE_MOTION_VECTORS_JITTER_CANCELLATION because
//           our motion vectors do NOT have jitter baked in.
//     NOTE: we do NOT set ENABLE_DEPTH_INFINITE because our scene has a
//           finite far plane (zFar = 100).
//
//  2. motionVectorScale = {1, 1}
//     Our MVs are in render-resolution pixel space (e.g. mv.x = 2.5 means
//     this pixel moved 2.5 pixels to the right). Scale = 1 tells FSR that
//     no further scaling is needed.
//
//  3. cameraNear = 0.1, cameraFar = 100 (actual scene values).
//     With ENABLE_DEPTH_INVERTED: FSR reads depth = near/z, where larger
//     values are closer to camera.
//
//  4. Shared resources (reconstructedPrevNearestDepth, dilatedDepth,
//     dilatedMotionVectors) are obtained from GetSharedResourceDescriptions
//     and managed by the application, not FSR internally.
//     reconstructedPrevNearestDepth must be cleared to 0 at the start of
//     every frame because FSR's PrepareInputs uses atomic max operations.
// =============================================================================
static void RunFsr3Sequence(
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
    void* scratchBuffer, size_t scratchBufferSize,
    bool sharpen,
    const std::string& outputName)
{
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    memset(scratchBuffer, 0, scratchBufferSize);

    // -------------------------------------------------------------------------
    // 1. Create backend interface
    // -------------------------------------------------------------------------
    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratchBuffer, scratchBufferSize, 4);

    // -------------------------------------------------------------------------
    // 2. Create FSR3 upscaler context
    //
    //    Flags chosen to match Cauldron's fsrapi sample, minus jitter-cancel
    //    (which Cauldron enables because it uses a full projection matrix with
    //    jitter baked into MVs — we instead supply raw pixel-space MVs without
    //    jitter, so we omit the flag).
    // -------------------------------------------------------------------------
    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    fsr3Desc.flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE  |
        FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE        |
        FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED       |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    // Note: ENABLE_DEPTH_INFINITE is omitted - our ray-caster has a finite far.
    // Note: ENABLE_MOTION_VECTORS_JITTER_CANCELLATION is omitted - our MVs
    //       do not contain jitter; they are true screen-space displacements.
    fsr3Desc.maxRenderSize    = { RENDER_W,  RENDER_H  };
    fsr3Desc.maxUpscaleSize   = { DISPLAY_W, DISPLAY_H };
    fsr3Desc.fpMessage        = Fsr3MsgCallback;
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
    std::cout << "[FSR3] Context created OK (sharpen=" << sharpen << ").\n";

    // -------------------------------------------------------------------------
    // 3. Query shared resource descriptions and create them
    // -------------------------------------------------------------------------
    FfxFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(ctx, &sharedDescs) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr3UpscalerGetSharedResourceDescriptions failed!\n";
        ffxFsr3UpscalerContextDestroy(ctx);
        _aligned_free(ctx);
        return;
    }

    SharedImage siRecon    = createSharedImage(device, physicalDevice,
                                               sharedDescs.reconstructedPrevNearestDepth);
    SharedImage siDilDepth = createSharedImage(device, physicalDevice,
                                               sharedDescs.dilatedDepth);
    SharedImage siDilMV    = createSharedImage(device, physicalDevice,
                                               sharedDescs.dilatedMotionVectors);

    // -------------------------------------------------------------------------
    // 4. Pre-loop: transition all intermediate images to GENERAL layout
    //    and the output image from UNDEFINED to GENERAL.
    // -------------------------------------------------------------------------
    {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transition(cmd, siRecon.image,    siRecon.layout,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
                   siRecon.info.mipLevels);
        transition(cmd, siDilDepth.image, siDilDepth.layout,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
                   siDilDepth.info.mipLevels);
        transition(cmd, siDilMV.image,    siDilMV.layout,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
                   siDilMV.info.mipLevels);

        VkImageLayout outLay = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, outputImage, outLay,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    // -------------------------------------------------------------------------
    // 5. Build resource descriptions (reused each frame)
    // -------------------------------------------------------------------------
    FfxResourceDescription colorDesc =
        ffxGetImageResourceDescriptionVK(colorImage,  colorInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc =
        ffxGetImageResourceDescriptionVK(depthImage,  depthInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription mvDesc    =
        ffxGetImageResourceDescriptionVK(mvImage,     mvInfo,     FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc   =
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription reconDesc =
        ffxGetImageResourceDescriptionVK(siRecon.image,    siRecon.info,    FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilDepthDesc =
        ffxGetImageResourceDescriptionVK(siDilDepth.image, siDilDepth.info, FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilMVDesc =
        ffxGetImageResourceDescriptionVK(siDilMV.image,    siDilMV.info,    FFX_RESOURCE_USAGE_UAV);

    // -------------------------------------------------------------------------
    // 6. Query jitter phase count
    // -------------------------------------------------------------------------
    int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(RENDER_W, DISPLAY_W);
    std::cout << "[FSR3] Jitter phase count: " << phaseCount << "\n";

    const int totalFrames = 128;
    std::cout << "[FSR3] Running " << totalFrames << " frames"
              << " (sharpen=" << sharpen << ")...\n";
    std::cout.flush();

    VkDeviceSize uploadSize = colorUploadSize + depthUploadSize + mvUploadSize;

    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    // reconRange covers all mip levels of reconstructedPrevNearestDepth
    VkImageSubresourceRange reconRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, siRecon.info.mipLevels, 0, 1 };

    int32_t jitterIndex = 0;
    float   prevJX = 0.f, prevJY = 0.f;
    void*   outData = nullptr;

    // -------------------------------------------------------------------------
    // 7. Main frame loop
    // -------------------------------------------------------------------------
    for (int i = 0; i < totalFrames; i++) {

        // 7a. Advance jitter index and get current jitter
        ++jitterIndex;
        float jX = 0.f, jY = 0.f;
        ffxFsr3UpscalerGetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

        // 7b. Render scene with correct jitter (NDC conversion done inside)
        std::vector<float> fColor(RENDER_W * RENDER_H * 4);
        std::vector<float> fDepth(RENDER_W * RENDER_H);
        std::vector<float> fMV   (RENDER_W * RENDER_H * 2);
        renderScene(RENDER_W, RENDER_H,
                    jX, jY,       // current frame jitter (pixel space)
                    prevJX, prevJY, // previous frame jitter (pixel space)
                    fColor.data(), fDepth.data(), fMV.data());

        // 7c. Upload to staging buffer
        void* mp;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mp);
        memcpy((uint8_t*)mp,                                     fColor.data(), colorUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize,                   fDepth.data(), depthUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, fMV.data(),    mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // 7d. Clear reconstructedPrevNearestDepth to zero every frame.
        //     FSR's PrepareInputs pass uses atomic max to build this buffer.
        //     Stale non-zero values from the previous frame corrupt the result.
        transition(cmd, siRecon.image, siRecon.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);
        VkClearColorValue zeroClear = {};
        vkCmdClearColorImage(cmd, siRecon.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zeroClear, 1, &reconRange);
        transition(cmd, siRecon.image, siRecon.layout,
                   VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_ASPECT_COLOR_BIT, siRecon.info.mipLevels);

        // 7e. Copy inputs from staging to GPU images
        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage,    mvLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy cR = {};
        cR.bufferOffset = 0;
        cR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cR);

        VkBufferImageCopy dR = {};
        dR.bufferOffset = colorUploadSize;
        dR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        dR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, depthImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dR);

        VkBufferImageCopy mR = {};
        mR.bufferOffset = colorUploadSize + depthUploadSize;
        mR.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        mR.imageExtent = { RENDER_W, RENDER_H, 1 };
        vkCmdCopyBufferToImage(cmd, uploadBuffer, mvImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mR);

        // 7f. Transition inputs to shader-read
        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage,    mvLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // 7g. Build FfxResource handles
        FfxResource colorRes = ffxGetResourceVK(colorImage,      colorDesc,
            L"FSR3_Color",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource depthRes = ffxGetResourceVK(depthImage,      depthDesc,
            L"FSR3_Depth",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource mvRes    = ffxGetResourceVK(mvImage,         mvDesc,
            L"FSR3_MVs",      FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource outRes   = ffxGetResourceVK(outputImage,     outDesc,
            L"FSR3_Output",   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource reconRes = ffxGetResourceVK(siRecon.image,    reconDesc,
            L"FSR3_Recon",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilDRes  = ffxGetResourceVK(siDilDepth.image, dilDepthDesc,
            L"FSR3_DilDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilMVRes = ffxGetResourceVK(siDilMV.image,   dilMVDesc,
            L"FSR3_DilMV",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        // 7h. Fill dispatch description
        FfxFsr3UpscalerDispatchDescription disp = {};
        disp.commandList   = ffxGetCommandListVK(cmd);
        disp.color         = colorRes;
        disp.depth         = depthRes;
        disp.motionVectors = mvRes;
        disp.output        = outRes;

        disp.reconstructedPrevNearestDepth = reconRes;
        disp.dilatedDepth                  = dilDRes;
        disp.dilatedMotionVectors          = dilMVRes;

        // Jitter in pixel space (as returned by ffxFsr3UpscalerGetJitterOffset)
        disp.jitterOffset.x = jX;
        disp.jitterOffset.y = jY;

        // motionVectorScale = {1, 1} because our MVs are already in
        // render-resolution pixel space. FSR will NOT multiply them further.
        disp.motionVectorScale.x = 1.0f;
        disp.motionVectorScale.y = 1.0f;

        disp.renderSize  = { RENDER_W,  RENDER_H  };
        disp.upscaleSize = { DISPLAY_W, DISPLAY_H };

        disp.enableSharpening        = sharpen;
        disp.sharpness               = 0.8f;
        disp.frameTimeDelta          = 16.6f;   // ~60 fps
        disp.preExposure             = 1.0f;
        disp.reset                   = (i == 0);

        // Camera parameters: match our scene exactly.
        // We use reversed-Z (ENABLE_DEPTH_INVERTED set) with a finite far plane.
        // Reversed-Z convention: cameraNear is the large NDC depth value (near),
        // cameraFar is the small NDC depth value (far).
        // The SDK documentation says: pass the actual distance values, not the
        // NDC values. FSR knows the buffer is inverted from the context flag.
        disp.cameraNear             = CAM_Z_NEAR;   // 0.1  (actual near distance)
        disp.cameraFar              = CAM_Z_FAR;    // 100  (actual far distance)
        disp.cameraFovAngleVertical = CAM_FOV_Y;    // 60 degrees in radians

        disp.viewSpaceToMetersFactor = 1.0f;
        disp.flags                   = 0;           // no per-dispatch debug flags

        // 7i. Dispatch FSR3 upscaler
        if (ffxFsr3UpscalerContextDispatch(ctx, &disp) != FFX_OK) {
            std::cout << "[FATAL] FSR3 dispatch failed frame " << i << "\n";
            break;
        }

        // 7j. Global memory barrier: ensure FSR writes are visible
        VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT  | VK_ACCESS_MEMORY_READ_BIT
                                 | VK_ACCESS_TRANSFER_READ_BIT;
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
        vkQueueWaitIdle(queue);

        // After FSR dispatch the output is left in GENERAL by the SDK
        outputLayout = VK_IMAGE_LAYOUT_GENERAL;

        // 7k. Snapshot frames
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
            transition(cmd, outputImage, outputLayout,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            vkEndCommandBuffer(cmd);
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
            std::string fname = "FSR_3.1.4_" +
                                std::string(sharpen ? "" : "norcas_") +
                                "frame" + std::to_string(frameNum) + ".png";
            saveFloatImage(fname, DISPLAY_W, DISPLAY_H, (float*)outData);
            vkUnmapMemory(device, downloadMemory);
            std::cout << "[FSR3] Snapshot frame " << frameNum << "\n";
        }

        if (frameNum % 32 == 0)
            std::cout << "[FSR3] Frame " << frameNum << "/" << totalFrames << "\n";

        prevJX = jX;
        prevJY = jY;
    }

    // -------------------------------------------------------------------------
    // 8. Final readback
    // -------------------------------------------------------------------------
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
    saveFloatImage(outputName, DISPLAY_W, DISPLAY_H, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    // -------------------------------------------------------------------------
    // 9. Cleanup
    // -------------------------------------------------------------------------
    ffxFsr3UpscalerContextDestroy(ctx);
    _aligned_free(ctx);

    vkDestroyImage(device, siRecon.image,     nullptr);
    vkFreeMemory  (device, siRecon.memory,    nullptr);
    vkDestroyImage(device, siDilDepth.image,  nullptr);
    vkFreeMemory  (device, siDilDepth.memory, nullptr);
    vkDestroyImage(device, siDilMV.image,     nullptr);
    vkFreeMemory  (device, siDilMV.memory,    nullptr);

    std::cout << "[FSR3] Sequence done -> " << outputName << "\n";
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

    // Each RunFsr3Sequence creates and destroys its own context.
    // Both calls use separately zeroed scratch buffers.
    size_t scratchSize2 = scratchBufferSize;
    void*  scratch2     = _aligned_malloc(scratchSize2, 64);
    memset(scratch2, 0, scratchSize2);

    // Run with sharpening enabled
    RunFsr3Sequence(
        device, physicalDevice, queue, cmd,
        uploadBuffer, uploadMemory,
        downloadBuffer, downloadMemory,
        colorImage, colorInfo,
        depthImage, depthInfo,
        mvImage,    mvInfo,
        outputImage, outputInfo,
        colorUploadSize, depthUploadSize, mvUploadSize, outSize,
        vkDevCtx, scratchBuffer, scratchBufferSize,
        true,  "FSR_3.1.4_2x.png");

    // Run without sharpening (for comparison)
    RunFsr3Sequence(
        device, physicalDevice, queue, cmd,
        uploadBuffer, uploadMemory,
        downloadBuffer, downloadMemory,
        colorImage, colorInfo,
        depthImage, depthInfo,
        mvImage,    mvInfo,
        outputImage, outputInfo,
        colorUploadSize, depthUploadSize, mvUploadSize, outSize,
        vkDevCtx, scratch2, scratchSize2,
        false, "FSR_3.1.4_norcas_2x.png");

    _aligned_free(scratch2);

    std::cout << "[FSR3] Done.\n";
}

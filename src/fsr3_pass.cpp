#include "common.h"

static void Fsr3MsgCallback(FfxMsgType /*type*/, const wchar_t* message) {
    if (!message) return;
    std::cout << "[FSR3 MSG] ";
    for (int i = 0; i < 2048 && message[i]; ++i)
        std::cout << (char)message[i];
    std::cout << "\n";
    std::cout.flush();
}

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

    FfxInterface ffxIface = {};
    ffxGetInterfaceVK(&ffxIface, ffxGetDeviceVK(&vkDevCtx),
                      scratchBuffer, scratchBufferSize, 4);

    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    fsr3Desc.flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE      |
        FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED     |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
        // No DEPTH_INFINITE: we have a real finite SCENE_ZFAR.
        // No MOTION_VECTORS_JITTER_CANCELLATION: MVs have no jitter in them.
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

    // Pre-loop initialisation: transition all shared images + output to GENERAL,
    // and clear reconstructedPrevNearestDepth ONCE to zero.
    // It must NOT be cleared again after this — it is a persistent inter-frame
    // resource. FSR3's PrepareInputs writes the current frame's depth into it;
    // FSR3's Accumulate reads it on the NEXT frame to validate history.
    // Clearing it every frame destroys that inter-frame data and prevents any
    // accumulation, causing a perpetually blurry single-frame output.
    {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        transition(cmd, siRecon.image,    siRecon.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                   siRecon.info.mipLevels);
        VkImageSubresourceRange reconRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, siRecon.info.mipLevels, 0, 1 };
        VkClearColorValue zeroClear = {};
        vkCmdClearColorImage(cmd, siRecon.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zeroClear, 1, &reconRange);
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

    FfxResourceDescription colorDesc =
        ffxGetImageResourceDescriptionVK(colorImage,  colorInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc =
        ffxGetImageResourceDescriptionVK(depthImage,  depthInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription mvDesc =
        ffxGetImageResourceDescriptionVK(mvImage,     mvInfo,     FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc =
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription reconDesc =
        ffxGetImageResourceDescriptionVK(siRecon.image,    siRecon.info,    FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilDepthDesc =
        ffxGetImageResourceDescriptionVK(siDilDepth.image, siDilDepth.info, FFX_RESOURCE_USAGE_UAV);
    FfxResourceDescription dilMVDesc =
        ffxGetImageResourceDescriptionVK(siDilMV.image,    siDilMV.info,    FFX_RESOURCE_USAGE_UAV);

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

    int32_t jitterIndex = 0;
    void*   outData     = nullptr;

    for (int i = 0; i < totalFrames; i++) {
        ++jitterIndex;
        float jX = 0.f, jY = 0.f;
        ffxFsr3UpscalerGetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

        std::vector<float> fColor(RENDER_W * RENDER_H * 4);
        std::vector<float> fDepth(RENDER_W * RENDER_H);
        std::vector<float> fMV   (RENDER_W * RENDER_H * 2, 0.f);
        renderScene(RENDER_W, RENDER_H, jX, jY,
                    fColor.data(), fDepth.data(), fMV.data());

        void* mp;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mp);
        memcpy((uint8_t*)mp,                                     fColor.data(), colorUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize,                   fDepth.data(), depthUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, fMV.data(),    mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // NOTE: Do NOT clear siRecon here. It is persistent across frames.
        // It was cleared once in the pre-loop block above (before frame 0).

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

        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage,    mvLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        FfxResource colorRes = ffxGetResourceVK(colorImage,  colorDesc,
            L"FSR3_Color",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource depthRes = ffxGetResourceVK(depthImage,  depthDesc,
            L"FSR3_Depth",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource mvRes    = ffxGetResourceVK(mvImage,     mvDesc,
            L"FSR3_MVs",      FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource outRes   = ffxGetResourceVK(outputImage, outDesc,
            L"FSR3_Output",   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource reconRes = ffxGetResourceVK(siRecon.image,    reconDesc,
            L"FSR3_Recon",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilDepthRes = ffxGetResourceVK(siDilDepth.image, dilDepthDesc,
            L"FSR3_DilDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        FfxResource dilMVRes = ffxGetResourceVK(siDilMV.image,    dilMVDesc,
            L"FSR3_DilMV",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        FfxFsr3UpscalerDispatchDescription disp = {};
        disp.commandList   = ffxGetCommandListVK(cmd);
        disp.color         = colorRes;
        disp.depth         = depthRes;
        disp.motionVectors = mvRes;
        disp.output        = outRes;

        disp.reconstructedPrevNearestDepth = reconRes;
        disp.dilatedDepth                  = dilDepthRes;
        disp.dilatedMotionVectors          = dilMVRes;

        disp.jitterOffset.x = jX;
        disp.jitterOffset.y = jY;

        // motionVectorScale = (1, 1): MVs are stored in pixel space directly.
        // (RENDER_W, RENDER_H) would only be correct for normalized MVs.)
        disp.motionVectorScale.x = 1.0f;
        disp.motionVectorScale.y = 1.0f;

        disp.renderSize  = { RENDER_W,  RENDER_H  };
        disp.upscaleSize = { DISPLAY_W, DISPLAY_H };

        disp.enableSharpening        = sharpen;
        disp.sharpness               = 0.8f;
        disp.frameTimeDelta          = 16.6f;
        disp.preExposure             = 1.f;
        disp.reset                   = (i == 0);
        disp.cameraNear              = SCENE_ZNEAR;
        disp.cameraFar               = SCENE_ZFAR;
        disp.cameraFovAngleVertical  = 1.04719755f;
        disp.viewSpaceToMetersFactor = 1.f;
        disp.flags                   = 0;

        if (ffxFsr3UpscalerContextDispatch(ctx, &disp) != FFX_OK) {
            std::cout << "[FATAL] FSR3 dispatch failed frame " << i << "\n";
            break;
        }

        VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                   VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT  |
                                   VK_ACCESS_MEMORY_READ_BIT  |
                                   VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
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

        outputLayout = VK_IMAGE_LAYOUT_GENERAL;

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
    }

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

    size_t scratchSize2 = scratchBufferSize;
    void*  scratch2     = _aligned_malloc(scratchSize2, 64);
    memset(scratch2, 0, scratchSize2);

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

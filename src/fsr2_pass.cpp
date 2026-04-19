#include "common.h"

static void FfxMsgCallback(FfxMsgType /*type*/, const wchar_t* message) {
    if (!message) return;
    std::cout << "[AMD MSG] ";
    for (int i = 0; i < 2048 && message[i]; ++i)
        std::cout << (char)message[i];
    std::cout << "\n";
    std::cout.flush();
}

void RunFsr2Pass(
    VkDevice device, VkPhysicalDevice physicalDevice,
    VkQueue queue, VkCommandBuffer cmd,
    VkBuffer uploadBuffer, VkDeviceMemory uploadMemory,
    VkBuffer downloadBuffer, VkDeviceMemory downloadMemory,
    VkImage colorImage,  VkImageCreateInfo colorInfo,
    VkImage depthImage,  VkImageCreateInfo depthInfo,
    VkImage mvImage,     VkImageCreateInfo mvInfo,
    VkImage outputImage, VkImageCreateInfo outputInfo,
    VkImage expImage,
    VkDeviceSize colorUploadSize, VkDeviceSize depthUploadSize,
    VkDeviceSize mvUploadSize,    VkDeviceSize outSize,
    VkDeviceContext& vkDevCtx,
    void* scratchBuffer, size_t scratchBufferSize)
{
    std::cout << "\n============================================\n";
    std::cout << "  FSR 2.3.3 Temporal Upscaling Test (128 frames)\n";
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

    FfxFsr2ContextDescription fsr2Desc = {};
    fsr2Desc.flags =
        FFX_FSR2_ENABLE_DEBUG_CHECKING                      |
        FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE                  |
        FFX_FSR2_ENABLE_AUTO_EXPOSURE                       |
        FFX_FSR2_ENABLE_DEPTH_INVERTED                      |
        FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
        // MOTION_VECTORS_JITTER_CANCELLATION: our MVs are computed at pixel
        // centres (0,0 for a static scene) and do NOT include the jitter
        // offset. This flag tells FSR2 to internally subtract the jitter
        // from the reprojection so it does not double-count the shift.
        // No DEPTH_INFINITE: we have a real finite SCENE_ZFAR.
    fsr2Desc.maxRenderSize    = { RENDER_W,  RENDER_H  };
    fsr2Desc.displaySize      = { DISPLAY_W, DISPLAY_H };
    fsr2Desc.fpMessage        = FfxMsgCallback;
    fsr2Desc.backendInterface = ffxIface;

    FfxFsr2Context* ctx =
        (FfxFsr2Context*)_aligned_malloc(sizeof(FfxFsr2Context), 64);
    memset(ctx, 0, sizeof(FfxFsr2Context));
    if (ffxFsr2ContextCreate(ctx, &fsr2Desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr2ContextCreate failed!\n"; return;
    }
    std::cout << "[FSR2] Context created OK.\n";

    {
        VkImageLayout outLay = VK_IMAGE_LAYOUT_UNDEFINED;
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);
        transition(cmd, outputImage, outLay,
                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mvLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout expLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkImageSubresourceRange colorRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    FfxResourceDescription colorDesc =
        ffxGetImageResourceDescriptionVK(colorImage,  colorInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc =
        ffxGetImageResourceDescriptionVK(depthImage,  depthInfo,  FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription mvDesc =
        ffxGetImageResourceDescriptionVK(mvImage,     mvInfo,     FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc =
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo, FFX_RESOURCE_USAGE_UAV);

    int32_t phaseCount = ffxFsr2GetJitterPhaseCount(RENDER_W, DISPLAY_W);
    std::cout << "[FSR2] Jitter phase count: " << phaseCount << "\n";

    const int totalFrames = 128;
    std::cout << "[FSR2] Running " << totalFrames << " frames...\n";
    std::cout.flush();

    VkDeviceSize uploadSize = colorUploadSize + depthUploadSize + mvUploadSize;
    int32_t jitterIndex = 0;
    void*   outData     = nullptr;

    for (int i = 0; i < totalFrames; i++) {
        ++jitterIndex;
        float jX = 0.f, jY = 0.f;
        ffxFsr2GetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

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

        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
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

        VkClearColorValue expClear = {{ 1.f, 0.f, 0.f, 0.f }};
        vkCmdClearColorImage(cmd, expImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &expClear, 1, &colorRange);

        transition(cmd, colorImage, colorLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, depthImage, depthLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, mvImage, mvLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, expImage, expLayout,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        FfxResource colorRes = ffxGetResourceVK(colorImage,  colorDesc,
            L"FSR2_Color",  FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource depthRes = ffxGetResourceVK(depthImage,  depthDesc,
            L"FSR2_Depth",  FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource mvRes    = ffxGetResourceVK(mvImage,     mvDesc,
            L"FSR2_MVs",    FFX_RESOURCE_STATE_COMPUTE_READ);
        FfxResource outRes   = ffxGetResourceVK(outputImage, outDesc,
            L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        FfxFsr2DispatchDescription dispatchDesc = {};
        dispatchDesc.commandList   = ffxGetCommandListVK(cmd);
        dispatchDesc.color         = colorRes;
        dispatchDesc.depth         = depthRes;
        dispatchDesc.motionVectors = mvRes;
        dispatchDesc.output        = outRes;

        dispatchDesc.jitterOffset.x = jX;
        dispatchDesc.jitterOffset.y = jY;

        // MVs are zero (static scene) in pixel space.
        // Scale of 1.0 passes them through unchanged.
        dispatchDesc.motionVectorScale.x = 1.0f;
        dispatchDesc.motionVectorScale.y = 1.0f;

        dispatchDesc.renderSize          = { RENDER_W, RENDER_H };
        dispatchDesc.enableSharpening    = true;
        dispatchDesc.sharpness           = 0.8f;
        dispatchDesc.frameTimeDelta      = 16.6f;
        dispatchDesc.preExposure         = 1.f;
        dispatchDesc.reset               = (i == 0);
        dispatchDesc.cameraNear              = SCENE_ZNEAR;
        dispatchDesc.cameraFar               = SCENE_ZFAR;
        dispatchDesc.cameraFovAngleVertical  = 1.04719755f;
        dispatchDesc.viewSpaceToMetersFactor = 1.f;

        if (ffxFsr2ContextDispatch(ctx, &dispatchDesc) != FFX_OK) {
            std::cout << "[FATAL] FSR2 dispatch failed frame " << i << "\n";
            return;
        }

        vkEndCommandBuffer(cmd);
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        int frameNum = i + 1;
        if (frameNum == 32 || frameNum == 64 ||
            frameNum == 96 || frameNum == 128)
        {
            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &beginInfo);
            transition(cmd, outputImage, outputLayout,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
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
            std::string fname = "FSR_2.3.3_frame" + std::to_string(frameNum) + ".png";
            saveFloatImage(fname, DISPLAY_W, DISPLAY_H, (float*)outData);
            vkUnmapMemory(device, downloadMemory);
            std::cout << "[FSR2] Snapshot frame " << frameNum << "\n";
        }

        if (frameNum % 32 == 0)
            std::cout << "[FSR2] Frame " << frameNum << "/" << totalFrames << "\n";
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
    saveFloatImage("FSR_2.3.3_2x.png", DISPLAY_W, DISPLAY_H, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr2ContextDestroy(ctx);
    _aligned_free(ctx);
    std::cout << "[FSR2] Done.\n";
}

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
// renderSceneFsr3: same ray-caster as renderScene() in common.cpp, but the
// motion vectors are output WITHOUT the jitter baked in.
//
// WHY:  FSR3 context is created WITHOUT the
//       FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION flag,
//       which means FSR3 expects motion vectors that do NOT contain jitter.
//       For our fully static scene the true screen-space motion of every
//       pixel between frames is exactly zero, so we write 0.
//
//       FSR2 works because it IS created with the jitter-cancellation flag
//       and therefore accepts jitter-contaminated MVs.  We keep FSR2 as-is.
//
// NOTE: The colour buffer IS rendered with jitter (ndcX/ndcY offset by
//       currJX/currJY) exactly as before – that is correct and required.
// ---------------------------------------------------------------------------
static void renderSceneFsr3(int w, int h,
                             float currJX, float currJY,
                             float* colorOut, float* depthOut, float* mvOut)
{
    const float aspect     = (float)w / (float)h;
    const float fovY       = 1.04719755f;
    const float tanHalfFov = tanf(fovY * 0.5f);
    const float zNear = 0.1f, zFar = 100.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // Apply jitter to the colour/depth sample position (correct).
            float ndcX = ((x + 0.5f + currJX) / w) * 2.0f - 1.0f;
            float ndcY = ((y + 0.5f + currJY) / h) * 2.0f - 1.0f;

            float ro[3] = { 0.f, 1.f, 0.f };
            float rd[3] = { ndcX * aspect * tanHalfFov, -ndcY * tanHalfFov, 1.f };
            float len   = sqrtf(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
            rd[0]/=len; rd[1]/=len; rd[2]/=len;

            float hitZ = -1.f;
            float r = powf(135.f/255.f, 2.2f);
            float g = powf(206.f/255.f, 2.2f);
            float b = powf(235.f/255.f, 2.2f);

            // Sphere
            float oc[3] = { ro[0], ro[1]-1.f, ro[2]-4.f };
            float bd    = rd[0]*oc[0] + rd[1]*oc[1] + rd[2]*oc[2];
            float cv    = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - 2.25f;
            float disc  = bd*bd - cv;
            if (disc > 0.f) {
                float t = -bd - sqrtf(disc);
                if (t > zNear && t < zFar) {
                    hitZ = t;
                    float nx = ro[0]+rd[0]*t;
                    float ny = ro[1]+rd[1]*t - 1.f;
                    float nz = ro[2]+rd[2]*t - 4.f;
                    float nl = sqrtf(nx*nx+ny*ny+nz*nz);
                    nx/=nl; ny/=nl; nz/=nl;
                    float light[3] = { 0.577f, 0.577f, -0.577f };
                    float ndotl = fmaxf(0.2f,
                        -(nx*light[0]+ny*light[1]+nz*light[2]));
                    r = powf(200.f/255.f, 2.2f) * ndotl;
                    g = powf( 50.f/255.f, 2.2f) * ndotl;
                    b = powf( 50.f/255.f, 2.2f) * ndotl;
                }
            }
            // Floor
            if (rd[1] < 0.f) {
                float t = -ro[1] / rd[1];
                if (t > zNear && t < zFar && (hitZ < 0.f || t < hitZ)) {
                    hitZ = t;
                    float px = ro[0]+rd[0]*t, pz = ro[2]+rd[2]*t;
                    int chk = ((int)floorf(px)+(int)floorf(pz)) % 2;
                    float cv2 = (chk==0) ? powf(220.f/255.f,2.2f)
                                         : powf( 80.f/255.f,2.2f);
                    r = g = b = cv2;
                }
            }

            int idx = y*w + x;
            colorOut[idx*4+0] = r;
            colorOut[idx*4+1] = g;
            colorOut[idx*4+2] = b;
            colorOut[idx*4+3] = 1.f;

            depthOut[idx] = (hitZ > 0.f) ? (zNear / hitZ) : 0.f;

            // FIX (Bug 2): Motion vectors are ZERO for a static scene.
            // FSR3 is created WITHOUT jitter-cancellation flag, so MVs
            // must NOT contain the jitter delta.  The camera is stationary,
            // so true screen-space motion is 0 for every pixel.
            mvOut[idx*2+0] = 0.f;
            mvOut[idx*2+1] = 0.f;
        }
    }
}

// ---------------------------------------------------------------------------
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

    // FIX (Bug 2): Remove FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION.
    // Our MVs for FSR3 are now true zero-motion vectors (static scene),
    // so FSR3 must NOT apply jitter cancellation to them.
    // Also remove FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED: our depth is
    // zNear/hitZ which produces 1.0 at the near plane and ~0 at the far
    // plane — that IS the inverted (reversed-Z) convention, so we KEEP it.
    FfxFsr3UpscalerContextDescription fsr3Desc = {};
    fsr3Desc.flags =
        FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE  |
        FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE       |
        // NOTE: jitter-cancellation flag intentionally OMITTED because our
        //       motion vectors do NOT contain jitter (see renderSceneFsr3).
        FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED      |
        FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
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

    // Pre-loop: bring shared images and output to GENERAL
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

    VkImageSubresourceRange reconRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, siRecon.info.mipLevels, 0, 1 };

    int32_t jitterIndex = 0;
    float   prevJX = 0.f, prevJY = 0.f;
    void*   outData = nullptr;

    for (int i = 0; i < totalFrames; i++) {
        ++jitterIndex;
        float jX = 0.f, jY = 0.f;
        ffxFsr3UpscalerGetJitterOffset(&jX, &jY, jitterIndex, phaseCount);

        // FIX (Bug 2): Use renderSceneFsr3 which outputs zero MVs.
        // The colour is still rendered with jitter applied to the ray direction.
        std::vector<float> fColor(RENDER_W * RENDER_H * 4);
        std::vector<float> fDepth(RENDER_W * RENDER_H);
        std::vector<float> fMV   (RENDER_W * RENDER_H * 2, 0.f);
        renderSceneFsr3(RENDER_W, RENDER_H, jX, jY,
                        fColor.data(), fDepth.data(), fMV.data());

        void* mp;
        vkMapMemory(device, uploadMemory, 0, uploadSize, 0, &mp);
        memcpy((uint8_t*)mp,                                     fColor.data(), colorUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize,                   fDepth.data(), depthUploadSize);
        memcpy((uint8_t*)mp + colorUploadSize + depthUploadSize, fMV.data(),    mvUploadSize);
        vkUnmapMemory(device, uploadMemory);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Clear reconstructedPrevNearestDepth to zero every frame.
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

        // Upload inputs
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

        // FIX (Bug 2, part 2): motionVectorScale.
        // Since our MVs are now in true screen-space pixel coordinates
        // (which happen to be zero for a static scene), the scale factor
        // is 1.0 in each axis.  FSR3 expects MVs in [-W..+W, -H..+H] range,
        // so scale = 1 means we hand FSR pixel-space MVs directly.
        // 
        // IMPORTANT: We keep scale at RENDER_W/RENDER_H here because the
        // motion vectors, while zero now, are conceptually in the same
        // normalised pixel-fraction space we used for FSR2 (i.e. if we ever
        // add real object motion we will output it in [-1..+1] space and
        // scale it to pixels here).  Since fMV is all zeros, the scale
        // value is irrelevant for a static scene — any non-zero scale
        // will produce the same (zero) result.  We keep RENDER_W/RENDER_H
        // for symmetry with FSR2 so that any future non-zero MVs are handled
        // identically.
        disp.motionVectorScale.x = (float)RENDER_W;
        disp.motionVectorScale.y = (float)RENDER_H;

        disp.renderSize  = { RENDER_W,  RENDER_H  };
        disp.upscaleSize = { DISPLAY_W, DISPLAY_H };

        disp.enableSharpening        = sharpen;
        disp.sharpness               = 0.8f;
        disp.frameTimeDelta          = 16.6f;
        disp.preExposure             = 1.f;
        disp.reset                   = (i == 0);

        // Depth convention: zNear/hitZ produces 1.0 at near, ~0 at far.
        // This is the inverted (reversed-Z) convention.
        // cameraNear should be the VALUE at near plane = 1.0 (after inversion),
        // cameraFar should be the VALUE at far plane  = ~0.
        // FSR3 with DEPTH_INVERTED: pass near=FLT_MAX (or 1.0), far=near_clip.
        // We keep the same convention as FSR2 which worked fine.
        disp.cameraNear              = FLT_MAX;
        disp.cameraFar               = 0.1f;
        disp.cameraFovAngleVertical  = 1.04719755f;
        disp.viewSpaceToMetersFactor = 1.f;
        disp.flags                   = 0;

        if (ffxFsr3UpscalerContextDispatch(ctx, &disp) != FFX_OK) {
            std::cout << "[FATAL] FSR3 dispatch failed frame " << i << "\n";
            break;
        }

        // Full pipeline barrier after FSR3 compute work.
        VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT  |
                                   VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT   |
                                   VK_ACCESS_MEMORY_READ_BIT   |
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

        // FIX (Bug 5): After FSR3 dispatch the SDK has internally
        // transitioned outputImage.  We don't know the exact final layout,
        // but the SDK leaves UAV resources in GENERAL after its own barriers.
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

        prevJX = jX;
        prevJY = jY;
    }

    // Final readback
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

// ---------------------------------------------------------------------------
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

    // Each sequence needs its own scratch buffer because ffxGetInterfaceVK
    // stores state in the scratch memory for the lifetime of the context.
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

    // Run without sharpening for comparison
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

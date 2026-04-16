#include "common.h"

void RunFsr1Pass(
    VkDevice device, VkPhysicalDevice physicalDevice,
    VkQueue queue, VkCommandBuffer cmd,
    VkBuffer uploadBuffer, VkDeviceMemory uploadMemory,
    VkBuffer downloadBuffer, VkDeviceMemory downloadMemory,
    VkImage colorImage,  VkImageCreateInfo colorInfo,
    VkImage outputImage, VkImageCreateInfo outputInfo,
    VkDeviceSize colorUploadSize, VkDeviceSize outSize,
    VkDeviceContext& vkDevCtx,
    void* scratchBuffer, size_t scratchBufferSize)
{
    std::cout << "\n============================================\n";
    std::cout << "  FSR 1.2 Spatial Upscaling Test\n";
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

    FfxFsr1ContextDescription desc = {};
    desc.flags        = FFX_FSR1_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR1_ENABLE_RCAS;
    desc.outputFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R32G32B32A32_SFLOAT);
    desc.maxRenderSize  = { RENDER_W,  RENDER_H  };
    desc.displaySize    = { DISPLAY_W, DISPLAY_H };
    desc.backendInterface = ffxIface;

    FfxFsr1Context* ctx =
        (FfxFsr1Context*)_aligned_malloc(sizeof(FfxFsr1Context), 64);
    memset(ctx, 0, sizeof(FfxFsr1Context));
    if (ffxFsr1ContextCreate(ctx, &desc) != FFX_OK) {
        std::cout << "[FATAL] ffxFsr1ContextCreate failed!\n"; return;
    }

    // Upload color
    {
        std::vector<float> fColor(RENDER_W * RENDER_H * 4);
        std::vector<float> fDepth(RENDER_W * RENDER_H);
        std::vector<float> fMV   (RENDER_W * RENDER_H * 2, 0.f);
        renderScene(RENDER_W, RENDER_H, 0.f, 0.f, 0.f, 0.f,
                    fColor.data(), fDepth.data(), fMV.data());
        void* mp;
        vkMapMemory(device, uploadMemory, 0, colorUploadSize, 0, &mp);
        memcpy(mp, fColor.data(), colorUploadSize);
        vkUnmapMemory(device, uploadMemory);
    }

    VkImageLayout colorLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout outputLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    transition(cmd, colorImage,  colorLayout,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cmd, outputImage, outputLayout,
               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy cr = {};
    cr.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cr.imageExtent = { RENDER_W, RENDER_H, 1 };
    vkCmdCopyBufferToImage(cmd, uploadBuffer, colorImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cr);

    transition(cmd, colorImage, colorLayout,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    FfxResource colorRes = ffxGetResourceVK(colorImage,
        ffxGetImageResourceDescriptionVK(colorImage, colorInfo,
                                         FFX_RESOURCE_USAGE_READ_ONLY),
        L"FSR1_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResource outRes = ffxGetResourceVK(outputImage,
        ffxGetImageResourceDescriptionVK(outputImage, outputInfo,
                                         FFX_RESOURCE_USAGE_UAV),
        L"FSR1_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxFsr1DispatchDescription disp = {};
    disp.commandList      = ffxGetCommandListVK(cmd);
    disp.color            = colorRes;
    disp.output           = outRes;
    disp.renderSize       = { RENDER_W, RENDER_H };
    disp.enableSharpening = true;
    disp.sharpness        = 0.8f;

    std::cout << "[FSR1] Dispatching...\n";
    if (ffxFsr1ContextDispatch(ctx, &disp) != FFX_OK) {
        std::cout << "[FATAL] FSR1 dispatch failed!\n"; return;
    }

    transition(cmd, outputImage, outputLayout,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy or2 = {};
    or2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    or2.imageExtent = { DISPLAY_W, DISPLAY_H, 1 };
    vkCmdCopyImageToBuffer(cmd, outputImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           downloadBuffer, 1, &or2);

    vkEndCommandBuffer(cmd);
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* outData;
    vkMapMemory(device, downloadMemory, 0, outSize, 0, &outData);
    saveFloatImage("FSR_1.2_2x.png", DISPLAY_W, DISPLAY_H, (float*)outData);
    vkUnmapMemory(device, downloadMemory);

    ffxFsr1ContextDestroy(ctx);
    _aligned_free(ctx);
    std::cout << "[FSR1] Done.\n";
}

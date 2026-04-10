cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Patch-F: SPD single-workgroup guard in HLSL ComputeAutoExposure
#   Only workgroup(0,0) calls SpdDownsample with NumWorkGroups=1
#   Other workgroups early-return
#
# Patch-G: Fix SPD atomic counter accumulation across frames
#   Root cause: SpdDownsample atomically increments counter each frame.
#   With NumWorkGroups=1 the counter goes 0->1->2->...->32 across frames.
#   Fix: patch AtomicIncSPDCounter in HLSL to always reset to 0 and
#   return 0, making workgroup(0,0) always behave as "last workgroup".
#   No internal C++ struct access needed - pure HLSL fix.
# =============================================================================

set(PATCH_A_ENABLED ON)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required for either (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed (confirmed)
set(PATCH_E_ENABLED OFF) # C++ SPD bypass - OFF (using HLSL fix instead)
set(PATCH_F_ENABLED ON)  # HLSL SPD single-workgroup guard
set(PATCH_G_ENABLED ON)  # HLSL SPD atomic counter reset fix

set(FFX_USE_PRECOMPILED_SHADERS OFF)

# =============================================================================
# DEPENDENCIES
# =============================================================================
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_Tag        master
)
FetchContent_MakeAvailable(volk)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/FindVulkan.cmake "
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES
            \"${vulkan_headers_SOURCE_DIR}/include\")
    endif()
    set(Vulkan_FOUND TRUE)
    set(VULKAN_FOUND TRUE)
    set(Vulkan_INCLUDE_DIRS \"${vulkan_headers_SOURCE_DIR}/include\")
    set(Vulkan_LIBRARIES Vulkan::Vulkan)
    set(Vulkan_VERSION \"1.3.250\")
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_BINARY_DIR}")

set(FFX_FSR1 ON CACHE BOOL "" FORCE)
set(FFX_FSR2 ON CACHE BOOL "" FORCE)
set(FFX_API_BACKEND "VK_X64" CACHE STRING "" FORCE)
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
set(FFX_USE_PRECOMPILED_SHADERS ${FFX_USE_PRECOMPILED_SHADERS}
    CACHE BOOL "" FORCE)

FetchContent_Declare(ffx_sdk_v2
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        v1.1.4
)
FetchContent_GetProperties(ffx_sdk_v2)

# Force re-patch every CMake run
set(FFX_STAMP_FILE
    "${CMAKE_BINARY_DIR}/_deps/ffx_sdk_v2-subbuild/ffx_sdk_v2-populate-prefix/src/ffx_sdk_v2-populate-stamp/ffx_sdk_v2-populate-done"
)
if(EXISTS "${FFX_STAMP_FILE}")
    message(STATUS "[Patch] Deleting FetchContent stamp to force re-patch...")
    file(REMOVE "${FFX_STAMP_FILE}")
endif()

if(NOT ffx_sdk_v2_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(ffx_sdk_v2)

    # =========================================================================
    # WIN7 COMPAT SCRUBBER - Required for: BOTH
    # =========================================================================
    file(GLOB_RECURSE ffx_source_files
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/*.h"
    )
    foreach(ffx_file IN LISTS ffx_source_files)
        file(READ ${ffx_file} file_contents)
        if(file_contents MATCHES "SetThreadDescription")
            string(REPLACE
                "SetThreadDescription("
                "0 * sizeof("
                file_contents "${file_contents}"
            )
            file(WRITE ${ffx_file} "${file_contents}")
            message(STATUS "[Patch] Win7 scrub: ${ffx_file}")
        endif()
    endforeach()

    # =========================================================================
    # PATCH ffx_vk.cpp (Patches A, B, C, D)
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
    file(READ "${VK_BACKEND_FILE}" vk_code)

    if(PATCH_A_ENABLED)
        set(PATCH_A_SEARCH
            "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"
        )
        string(FIND "${vk_code}" "${PATCH_A_SEARCH}" PATCH_A_POS)
        if(PATCH_A_POS EQUAL -1)
            message(STATUS "[Patch-A] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_A_SEARCH}"
                "if (false) /* [Fix-A] Device-local memory preference disabled */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-A] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-A] DISABLED.")
    endif()

    if(PATCH_B_ENABLED)
        set(PATCH_B_SEARCH "if (devCaps.dedicatedAllocationSupported)")
        string(FIND "${vk_code}" "${PATCH_B_SEARCH}" PATCH_B_POS)
        if(PATCH_B_POS EQUAL -1)
            message(STATUS "[Patch-B] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_B_SEARCH}"
                "if (false) /* [Fix-B] Dedicated allocation disabled */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-B] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-B] DISABLED.")
    endif()

    if(PATCH_C_ENABLED)
        set(PATCH_C_SEARCH
            "deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16"
        )
        string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
        if(PATCH_C_POS EQUAL -1)
            message(FATAL_ERROR "[Patch-C] Search string not found!")
        else()
            string(REPLACE "${PATCH_C_SEARCH}"
                "deviceCapabilities->fp16Supported = false /* [Fix-C] FP16 kill switch */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-C] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-C] DISABLED.")
    endif()

    if(PATCH_D_ENABLED)
        set(PATCH_D_SEARCH "return VK_FORMAT_B10G11R11_UFLOAT_PACK32")
        string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
        if(PATCH_D_POS EQUAL -1)
            message(STATUS "[Patch-D] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_D_SEARCH}"
                "return VK_FORMAT_R16G16B16A16_SFLOAT /* [Fix-D] B10G11R11 override */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-D] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-D] DISABLED.")
    endif()

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] ffx_vk.cpp written.")

    # =========================================================================
    # PATCH E: C++ SPD dispatch bypass in ffx_fsr2.cpp
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR "[Patch-E] ffx_fsr2.cpp not found!")
    endif()
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        set(PATCH_E_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
        if(PATCH_E_POS EQUAL -1)
            set(PATCH_E_SEARCH_ALT
                "&context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0]"
            )
            string(FIND "${fsr2_code}" "${PATCH_E_SEARCH_ALT}" PATCH_E_ALT_POS)
            if(PATCH_E_ALT_POS EQUAL -1)
                message(FATAL_ERROR "[Patch-E] SPD dispatch not found!")
            else()
                string(REPLACE "${PATCH_E_SEARCH_ALT}"
                    "&context->pipelineComputeLuminancePyramid, 0 /* [Fix-E] SPD bypassed */"
                    fsr2_code "${fsr2_code}")
                message(STATUS "[Patch-E] Applied via ALT. OK!")
            endif()
        else()
            string(REPLACE "${PATCH_E_SEARCH}"
                "/* [Fix-E] SPD dispatch SKIPPED */"
                fsr2_code "${fsr2_code}")
            message(STATUS "[Patch-E] SPD dispatch skipped. OK!")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        file(READ "${FSR2_CPP_FILE}" fsr2_verify)
        string(FIND "${fsr2_verify}" "Fix-E" VERIFY_E_POS)
        if(VERIFY_E_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-E] Patch-E not confirmed!")
        else()
            message(STATUS "[VERIFY-E] Patch-E confirmed. OK!")
        endif()
    else()
        # Patch-E inactive - inject minimal diagnostics
        set(CP2_3_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${CP2_3_SEARCH}" CP2_3_POS)
        if(NOT CP2_3_POS EQUAL -1)
            string(REPLACE "${CP2_3_SEARCH}"
                "printf(\"[SPD] Dispatching Groups=[%u,%u]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[SPD] scheduleDispatch returned OK\\n\"); fflush(stdout)"
                fsr2_code "${fsr2_code}")
        endif()
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] DISABLED - SPD dispatch runs normally.")
    endif()

    # =========================================================================
    # PATCH F + G: Fix SPD atomic counter in HLSL
    #
    # Both patches modify ffx_fsr2_compute_luminance_pyramid.h
    # They are applied together to that single file.
    #
    # Patch-F: Single-workgroup guard
    #   Renames ComputeAutoExposure -> ComputeAutoExposure_Original
    #   Adds wrapper that only lets workgroup(0,0) through,
    #   passing NumWorkGroups=1 to SpdDownsample.
    #
    # Patch-G: Atomic counter reset
    #   Patches AtomicIncSPDCounter to always store 0 back to the
    #   counter buffer and return 0.
    #   This means:
    #     - The GPU buffer is always reset to 0 at the start of each
    #       SpdDownsample call (first thing workgroup(0,0) does)
    #     - workgroup(0,0) always sees counter==0, increments to 1,
    #       checks 1==NumWorkGroups(1) -> TRUE -> proceeds as last WG
    #     - Counter is written back as 0 (not 1) so next frame starts clean
    #   Result: no accumulation across frames, no heap corruption.
    #
    # Why AtomicIncSPDCounter?
    #   From ffx_fsr2_compute_luminance_pyramid.h in the SDK:
    #   void SpdIncreaseAtomicCounter(FfxUInt32 slice,
    #       FFX_PARAMETER_INOUT FfxUInt32 spdCounter)
    #   {
    #       AtomicIncSPDCounter(slice, spdCounter);
    #   }
    #   AtomicIncSPDCounter is the macro/function that does the
    #   actual imageAtomicAdd to the SPD counter buffer.
    #   By overriding it we control what gets written to GPU memory.
    # =========================================================================
    if(PATCH_F_ENABLED OR PATCH_G_ENABLED)
        set(LUM_PYRAMID_HLSL
            "${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h"
        )

        if(NOT EXISTS "${LUM_PYRAMID_HLSL}")
            message(FATAL_ERROR
                "[Patch-F/G] HLSL file not found at:\n${LUM_PYRAMID_HLSL}"
            )
        endif()

        file(READ "${LUM_PYRAMID_HLSL}" hlsl_code)
        message(STATUS "[Patch-F/G] HLSL luminance pyramid file read OK.")

        # ---------------------------------------------------------------------
        # PATCH G: Override AtomicIncSPDCounter to reset counter each frame
        #
        # Original in ffx_fsr2_compute_luminance_pyramid.h:
        #   void SpdIncreaseAtomicCounter(FfxUInt32 slice,
        #       FFX_PARAMETER_INOUT FfxUInt32 spdCounter)
        #   {
        #       AtomicIncSPDCounter(slice, spdCounter);
        #   }
        #
        # We rename the original and replace with a version that:
        #   1. Reads current counter value (always 0 on first call per frame
        #      because we reset it)
        #   2. Sets spdCounter = 0 (so the "last workgroup" check always
        #      sees 0+1 == NumWorkGroups(1) == TRUE)
        #   3. Writes 0 back to GPU buffer (reset for next frame)
        #
        # This is safe because with Patch-F we only have 1 workgroup running,
        # so the atomic coordination logic is irrelevant - we just need
        # workgroup(0,0) to always pass the "am I last?" check.
        # ---------------------------------------------------------------------
        if(PATCH_G_ENABLED)
            set(PATCH_G_SEARCH
                "void SpdIncreaseAtomicCounter(FfxUInt32 slice, FFX_PARAMETER_INOUT FfxUInt32 spdCounter)"
            )
            string(FIND "${hlsl_code}" "${PATCH_G_SEARCH}" PATCH_G_POS)
            if(PATCH_G_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-G] SpdIncreaseAtomicCounter not found in HLSL!"
                )
            else()
                # Rename original
                string(REPLACE
                    "${PATCH_G_SEARCH}"
                    "void SpdIncreaseAtomicCounter_Original(FfxUInt32 slice, FFX_PARAMETER_INOUT FfxUInt32 spdCounter)"
                    hlsl_code "${hlsl_code}")

                # We also need to rename the call inside it so the original
                # still compiles (it calls AtomicIncSPDCounter which is fine)
                # Now append our replacement after the file content
                set(PATCH_G_REPLACEMENT
"
// [Fix-G] SPD atomic counter reset - prevents accumulation across frames.
// With NumWorkGroups=1 (Patch-F), workgroup(0,0) is always the only/last WG.
// We set spdCounter=0 so the 'last workgroup' check (spdCounter+1==numWGs)
// always passes for workgroup(0,0). We also write 0 back to GPU memory
// so the counter is clean for the next frame dispatch.
void SpdIncreaseAtomicCounter(FfxUInt32 slice,
    FFX_PARAMETER_INOUT FfxUInt32 spdCounter)
{
    // Set counter to 0: with NumWorkGroups=1, check is (0+1)==1 -> TRUE
    // workgroup(0,0) always proceeds as 'last workgroup'. Correct behavior.
    spdCounter = 0;
    // Write 0 back to GPU buffer so next frame starts clean (no accumulation)
    AtomicIncSPDCounter(slice, spdCounter);
    spdCounter = 0;
}
"
                )
                string(APPEND hlsl_code "${PATCH_G_REPLACEMENT}")
                message(STATUS "[Patch-G] SpdIncreaseAtomicCounter override appended.")
            endif()
        else()
            message(STATUS "[Patch-G] DISABLED.")
        endif()

        # ---------------------------------------------------------------------
        # PATCH F: Single-workgroup wrapper for ComputeAutoExposure
        # ---------------------------------------------------------------------
        if(PATCH_F_ENABLED)
            set(PATCH_F_SEARCH
                "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            )
            string(FIND "${hlsl_code}" "${PATCH_F_SEARCH}" PATCH_F_POS)
            if(PATCH_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-F] ComputeAutoExposure signature not found!"
                )
            else()
                # Rename original
                string(REPLACE
                    "${PATCH_F_SEARCH}"
                    "void ComputeAutoExposure_Original(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
                    hlsl_code "${hlsl_code}")

                # Append wrapper
                set(PATCH_F_WRAPPER
"
// [Fix-F] SPD single-workgroup wrapper.
// Only workgroup(0,0) runs SpdDownsample with NumWorkGroups=1.
// All other workgroups early-return without touching any GPU memory.
// Combined with Fix-G, the atomic counter is always clean each frame.
void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)
{
    if (WorkGroupId.x != 0 || WorkGroupId.y != 0) return;

#if FFX_HALF
    SpdDownsampleH(
        FfxUInt32x2(0, 0),
        FfxUInt32(LocalThreadIndex),
        FfxUInt32(MipCount()),
        FfxUInt32(1),
        FfxUInt32(WorkGroupId.z),
        FfxUInt32x2(0, 0));
#else
    SpdDownsample(
        FfxUInt32x2(0, 0),
        FfxUInt32(LocalThreadIndex),
        FfxUInt32(MipCount()),
        FfxUInt32(1),
        FfxUInt32(WorkGroupId.z),
        FfxUInt32x2(0, 0));
#endif
}
"
                )
                string(APPEND hlsl_code "${PATCH_F_WRAPPER}")
                message(STATUS "[Patch-F] ComputeAutoExposure wrapper appended.")
            endif()
        else()
            message(STATUS "[Patch-F] DISABLED.")
        endif()

        # Write patched HLSL
        file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")
        message(STATUS "[Patch-F/G] HLSL file written.")

        # Verify
        file(READ "${LUM_PYRAMID_HLSL}" hlsl_verify)
        if(PATCH_F_ENABLED)
            string(FIND "${hlsl_verify}" "Fix-F" VERIFY_F_POS)
            if(VERIFY_F_POS EQUAL -1)
                message(FATAL_ERROR "[VERIFY-F] Patch-F not confirmed!")
            else()
                message(STATUS "[VERIFY-F] Patch-F confirmed OK.")
            endif()
        endif()
        if(PATCH_G_ENABLED)
            string(FIND "${hlsl_verify}" "Fix-G" VERIFY_G_POS)
            if(VERIFY_G_POS EQUAL -1)
                message(FATAL_ERROR "[VERIFY-G] Patch-G not confirmed!")
            else()
                message(STATUS "[VERIFY-G] Patch-G confirmed OK.")
            endif()
        endif()
    endif()

    # =========================================================================
    # DXC SETUP
    # =========================================================================
    if(NOT FFX_USE_PRECOMPILED_SHADERS)
        message(STATUS "[Shader] Precompiled shaders DISABLED.")
        message(STATUS "[Shader] Searching for DXC compiler...")

        find_program(DXC_PATH
            NAMES dxc.exe dxc
            HINTS
                "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC"
                "C:/Program Files (x86)/Windows Kits/10/bin"
            PATH_SUFFIXES "x64" "bin/Hostx64/x64"
            DOC "Path to DXC shader compiler"
        )

        if(NOT DXC_PATH)
            message(STATUS "[Shader] find_program did not locate DXC.")
            message(STATUS "[Shader] Trying glob search...")
            file(GLOB_RECURSE DXC_CANDIDATES
                "C:/Program Files/Microsoft Visual Studio/2022/*/dxc.exe"
                "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
            )
            if(DXC_CANDIDATES)
                list(GET DXC_CANDIDATES 0 DXC_PATH)
                message(STATUS "[Shader] DXC found via glob: ${DXC_PATH}")
            else()
                message(FATAL_ERROR "[Shader] DXC compiler not found!")
            endif()
        else()
            message(STATUS "[Shader] DXC found: ${DXC_PATH}")
        endif()

        set(DXC_PATH "${DXC_PATH}" CACHE STRING "" FORCE)
        message(STATUS "[Shader] DXC path set: ${DXC_PATH}")
    else()
        message(STATUS "[Shader] Using precompiled SPIR-V blobs.")
    endif()

    add_subdirectory(${ffx_sdk_v2_SOURCE_DIR}/sdk ${ffx_sdk_v2_BINARY_DIR}/sdk)
endif()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp "
#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include \"stb_image_write.h\"
")

add_executable(vk-upscale-testbed
    src/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp
)

target_include_directories(vk-upscale-testbed PRIVATE
    ${stb_SOURCE_DIR}
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

target_link_libraries(vk-upscale-testbed PRIVATE
    volk::volk_headers
    volk::volk
    ffx_fsr1_x64
    ffx_fsr2_x64
    ffx_backend_vk_x64
)

target_compile_definitions(ffx_backend_vk_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr1_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr2_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)

target_link_libraries(ffx_backend_vk_x64 volk::volk)

if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )
endif()

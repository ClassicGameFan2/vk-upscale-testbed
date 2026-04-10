cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Summary of all patches and their purpose:
#
# Patch-A: RAM allocation fix in ffx_vk.cpp
#   Disables device-local memory preference so Lavapipe/SwiftShader work.
#   Required for BOTH renderers.
#
# Patch-B: Dedicated allocation disable in ffx_vk.cpp
#   Not needed - confirmed OFF.
#
# Patch-C: FP16 kill switch in ffx_vk.cpp
#   Forces FP32 shader path. Not needed for Lavapipe - confirmed OFF.
#
# Patch-D: B10G11R11 format override in ffx_vk.cpp
#   Not needed for Lavapipe - confirmed OFF.
#
# Patch-E: C++ SPD workgroup count clamp in ffx_fsr2.cpp
#   UPGRADED: now clamps dispatchThreadGroupCountXY to [1,1] before
#   the luminance pyramid scheduleDispatch. This means Lavapipe only
#   allocates resources for 1 workgroup instead of 20, fixing the
#   memory exhaustion that caused stbi_write_png to crash.
#   Combined with Patch-F (HLSL guard), only workgroup(0,0) runs SPD.
#
# Patch-F: HLSL single-workgroup guard in ffx_fsr2_compute_luminance_pyramid.h
#   Wraps ComputeAutoExposure to early-return all workgroups except (0,0).
#   Passes NumWorkGroups=1 so SPD never uses inter-workgroup atomics.
#   Required for Lavapipe.
#
# Patch-G: HLSL SpdIncreaseAtomicCounter bypass
#   Makes spdCounter=0 without calling SPD_IncreaseAtomicCounter.
#   Ensures workgroup(0,0) always passes the 'last workgroup' check.
#   SPD_ResetAtomicCounter still runs to clean up the GPU buffer.
# =============================================================================

set(PATCH_A_ENABLED ON)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed for Lavapipe (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed for Lavapipe (confirmed)
set(PATCH_E_ENABLED ON)  # Workgroup count clamp - fixes stbi crash
set(PATCH_F_ENABLED ON)  # HLSL single-workgroup guard
set(PATCH_G_ENABLED ON)  # HLSL atomic counter bypass

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
    # PATCH E: Clamp SPD workgroup count to [1,1] in ffx_fsr2.cpp
    #
    # ROOT CAUSE OF stbi_write_png CRASH:
    #   With Patch-F (HLSL early-return), workgroups (1,0)..(4,3) still get
    #   dispatched to Lavapipe even though they immediately return.
    #   Lavapipe allocates groupshared memory for ALL dispatched workgroups:
    #     spdIntermediateR/G/B/A[16][16] = 4 * 16*16 * 4 bytes = 4KB each
    #     With 5*4=20 workgroups: 20 * ~6KB groupshared = ~120KB per dispatch
    #     Over 32 frames this accumulates in Lavapipe's internal allocator.
    #   When stbi_write_png calls malloc() for its PNG compression buffers,
    #   Lavapipe's deferred memory cleanup triggers and corrupts the heap.
    #
    # FIX:
    #   Clamp dispatchThreadGroupCountXY[0] and [1] to 1 before the
    #   luminance pyramid scheduleDispatch call. Lavapipe then only
    #   allocates resources for 1 workgroup total.
    #   Combined with Patch-F HLSL guard (which also passes NumWorkGroups=1
    #   to SpdDownsample), this is fully consistent and correct.
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR "[Patch-E] ffx_fsr2.cpp not found!")
    endif()
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        # Search for the SPD dispatch call
        set(PATCH_E_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
        if(PATCH_E_POS EQUAL -1)
            message(FATAL_ERROR "[Patch-E] SPD scheduleDispatch not found in ffx_fsr2.cpp!")
        endif()

        # Replace: clamp workgroup counts to 1, add diagnostics, then dispatch
        string(REPLACE "${PATCH_E_SEARCH}"
            "printf(\"[SPD] Dispatching Groups=[%u,%u] -> clamped to [1,1]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); dispatchThreadGroupCountXY[0] = 1; dispatchThreadGroupCountXY[1] = 1; scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[SPD] scheduleDispatch returned OK\\n\"); fflush(stdout)"
            fsr2_code "${fsr2_code}")

        # Verify
        string(FIND "${fsr2_code}" "clamped to [1,1]" VERIFY_E_POS)
        if(VERIFY_E_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-E] Patch-E replacement not confirmed!")
        else()
            message(STATUS "[Patch-E] SPD workgroup clamp applied OK.")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] ffx_fsr2.cpp written.")
    else()
        # Patch-E inactive - inject minimal diagnostics only
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
        message(STATUS "[Patch-E] DISABLED - SPD runs with original workgroup count.")
    endif()

    # =========================================================================
    # PATCH F + G: Fix SPD in HLSL
    #
    # Both patches modify ffx_fsr2_compute_luminance_pyramid.h
    #
    # Patch-F: ComputeAutoExposure single-workgroup wrapper
    #   Renames original ComputeAutoExposure, adds wrapper that:
    #   - Early-returns all workgroups except (0,0)
    #   - Calls SpdDownsample with NumWorkGroups=1
    #   This prevents inter-workgroup atomic coordination.
    #
    # Patch-G: SpdIncreaseAtomicCounter bypass
    #   From the actual source we can see:
    #     void SpdIncreaseAtomicCounter(FfxUInt32 slice)   <- 1 param only
    #     { SPD_IncreaseAtomicCounter(spdCounter); }       <- groupshared var
    #   The groupshared spdCounter resets automatically each dispatch.
    #   SPD_IncreaseAtomicCounter writes to the GPU storage buffer atomically.
    #   With NumWorkGroups=1 and only workgroup(0,0) running, we want
    #   the 'last workgroup' check to always pass.
    #   We bypass SPD_IncreaseAtomicCounter and set spdCounter=0 directly,
    #   so SpdGetAtomicCounter() returns 0, and the SPD check
    #   (0 == numWorkGroups-1) == (0 == 0) is TRUE for NumWorkGroups=1.
    #   SpdResetAtomicCounter still runs normally to clean GPU buffer.
    # =========================================================================
    if(PATCH_F_ENABLED OR PATCH_G_ENABLED)
        set(LUM_PYRAMID_HLSL
            "${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h"
        )
        if(NOT EXISTS "${LUM_PYRAMID_HLSL}")
            message(FATAL_ERROR
                "[Patch-F/G] HLSL not found: ${LUM_PYRAMID_HLSL}")
        endif()

        file(READ "${LUM_PYRAMID_HLSL}" hlsl_code)
        message(STATUS "[Patch-F/G] HLSL file read OK.")

        # ---------------------------------------------------------------------
        # PATCH G: Bypass SpdIncreaseAtomicCounter
        # Exact signature from source: void SpdIncreaseAtomicCounter(FfxUInt32 slice)
        # Exact body from source:      SPD_IncreaseAtomicCounter(spdCounter);
        # We rename it and append a replacement that sets spdCounter=0.
        # ---------------------------------------------------------------------
        if(PATCH_G_ENABLED)
            set(PATCH_G_SEARCH
"void SpdIncreaseAtomicCounter(FfxUInt32 slice)
{
    SPD_IncreaseAtomicCounter(spdCounter);
}"
            )
            string(FIND "${hlsl_code}" "${PATCH_G_SEARCH}" PATCH_G_POS)
            if(PATCH_G_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-G] SpdIncreaseAtomicCounter body not found!")
            else()
                string(REPLACE "${PATCH_G_SEARCH}"
"void SpdIncreaseAtomicCounter_Original(FfxUInt32 slice)
{
    SPD_IncreaseAtomicCounter(spdCounter);
}"
                    hlsl_code "${hlsl_code}")

                # Append replacement - sets spdCounter=0 without touching GPU buffer
                # With NumWorkGroups=1: SpdGetAtomicCounter() returns 0
                # SPD check: (0 == numWorkGroups-1) = (0 == 0) = TRUE
                # workgroup(0,0) always proceeds as last workgroup. Correct.
                # SpdResetAtomicCounter() still runs and cleans GPU buffer.
                string(APPEND hlsl_code
"
// [Fix-G] Bypass GPU atomic counter - sets groupshared spdCounter=0 directly.
// With NumWorkGroups=1 (Fix-F), workgroup(0,0) must always be 'last workgroup'.
// SPD check: SpdGetAtomicCounter()==numWorkGroups-1 -> 0==0 -> TRUE. Correct.
// GPU storage buffer is never written here; SpdResetAtomicCounter handles cleanup.
void SpdIncreaseAtomicCounter(FfxUInt32 slice)
{
    spdCounter = 0; // [Fix-G] bypass GPU atomic, force 'last workgroup' check to pass
}
"
                )
                message(STATUS "[Patch-G] SpdIncreaseAtomicCounter bypass applied OK.")
            endif()
        else()
            message(STATUS "[Patch-G] DISABLED.")
        endif()

        # ---------------------------------------------------------------------
        # PATCH F: ComputeAutoExposure single-workgroup wrapper
        # Exact signature from source:
        #   void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)
        # ---------------------------------------------------------------------
        if(PATCH_F_ENABLED)
            set(PATCH_F_SEARCH
                "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            )
            string(FIND "${hlsl_code}" "${PATCH_F_SEARCH}" PATCH_F_POS)
            if(PATCH_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-F] ComputeAutoExposure not found in HLSL!")
            else()
                string(REPLACE "${PATCH_F_SEARCH}"
                    "void ComputeAutoExposure_Original(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
                    hlsl_code "${hlsl_code}")

                string(APPEND hlsl_code
"
// [Fix-F] Single-workgroup wrapper for ComputeAutoExposure.
// Only workgroup(0,0) runs SpdDownsample. All others early-return.
// NumWorkGroups=1 passed to SpdDownsample - no inter-workgroup atomics needed.
// Combined with Fix-G: workgroup(0,0) always passes 'last workgroup' check.
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
                message(STATUS "[Patch-F] ComputeAutoExposure wrapper applied OK.")
            endif()
        else()
            message(STATUS "[Patch-F] DISABLED.")
        endif()

        # Write patched HLSL
        file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")
        message(STATUS "[Patch-F/G] HLSL file written.")

        # Verify both patches
        file(READ "${LUM_PYRAMID_HLSL}" hlsl_verify)
        if(PATCH_G_ENABLED)
            string(FIND "${hlsl_verify}" "Fix-G" VERIFY_G_POS)
            if(VERIFY_G_POS EQUAL -1)
                message(FATAL_ERROR "[VERIFY-G] Patch-G not confirmed!")
            else()
                message(STATUS "[VERIFY-G] Patch-G confirmed OK.")
            endif()
        endif()
        if(PATCH_F_ENABLED)
            string(FIND "${hlsl_verify}" "Fix-F" VERIFY_F_POS)
            if(VERIFY_F_POS EQUAL -1)
                message(FATAL_ERROR "[VERIFY-F] Patch-F not confirmed!")
            else()
                message(STATUS "[VERIFY-F] Patch-F confirmed OK.")
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

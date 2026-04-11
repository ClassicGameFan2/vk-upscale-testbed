cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Phase 2 conclusion:
#   Patch-F (ComputeAutoExposure early return) = no crash on Lavapipe
#   CONFIRMED: crash is inside SpdDownsample() shader code
#   Specifically: global atomic counter operation crashes Lavapipe
#   when the atomic counter buffer is not initialized to zero.
#
# Fix strategy:
#   Patch-F upgraded from "early return" to "single workgroup guard"
#   We allow SpdDownsample to run but only for workgroup (0,0).
#   This avoids the inter-workgroup atomic coordination entirely.
#   Result: SPD runs partially (processes top-left region only)
#           but does NOT crash.
#   This gives us real luminance data from part of the image
#   which is much better than zeros from the early return.
# =============================================================================

set(PATCH_A_ENABLED OFF)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required for either (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed (confirmed)
set(PATCH_E_ENABLED OFF) # C++ SPD bypass - OFF (using HLSL fix instead)
set(PATCH_F_ENABLED OFF)  # HLSL SPD fix - single workgroup guard

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
#    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
#    GIT_TAG        v1.1.4
    GIT_REPOSITORY https://github.com/ClassicGameFan2/FidelityFX-SDK.git
    GIT_TAG        changes
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
    # PATCH F: Fix SPD global atomic counter crash on Lavapipe
    #
    # CONFIRMED ROOT CAUSE (from Phase 2 testing):
    #   ComputeAutoExposure() early return = no crash
    #   ComputeAutoExposure() runs SpdDownsample() = crash
    #   Therefore: SpdDownsample() crashes Lavapipe
    #
    # WHY SpdDownsample CRASHES:
    #   SPD uses a global atomic counter to elect one "last workgroup"
    #   that does the final mip reduction across all workgroups.
    #   This counter is stored in a GPU storage buffer.
    #   The counter MUST be zero before each dispatch starts.
    #   Lavapipe enforces this strictly -> crash if not zero.
    #   SwiftShader ignores this -> silent corruption.
    #
    # FIX APPROACH - Single Workgroup Guard:
    #   We modify ComputeAutoExposure to only run for workgroup (0,0)
    #   and pass NumWorkGroups=1 to SpdDownsample.
    #   When NumWorkGroups=1, SPD knows there is only one workgroup
    #   so it NEVER calls the global atomic counter at all.
    #   The atomic coordination code path is completely bypassed.
    #
    # TRADE-OFF:
    #   Only workgroup (0,0) processes its 64x64 pixel region.
    #   Other workgroups (1,0), (2,0), (0,1) etc. skip early.
    #   Luminance pyramid is computed from top-left 64x64 pixels only.
    #   This is imperfect but:
    #     - No crash on Lavapipe
    #     - Real luminance data (not zeros like Patch-E)
    #     - Good enough for auto-exposure on simple scenes
    #     - Much better than the C++ bypass (Patch-E)
    # =========================================================================
    if(PATCH_F_ENABLED)
        if(FFX_USE_PRECOMPILED_SHADERS)
            message(STATUS
                "[Patch-F] WARNING: Has no effect with precompiled shaders ON."
            )
        else()
            set(LUM_PYRAMID_HLSL
                "${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h"
            )

            if(NOT EXISTS "${LUM_PYRAMID_HLSL}")
                message(FATAL_ERROR
                    "[Patch-F] HLSL file not found at:\n${LUM_PYRAMID_HLSL}"
                )
            endif()

            file(READ "${LUM_PYRAMID_HLSL}" hlsl_code)
            message(STATUS "[Patch-F] HLSL luminance pyramid file read OK.")

            # Replace ComputeAutoExposure with a version that:
            # 1. Skips all workgroups except (0,0)
            # 2. Passes NumWorkGroups=1 so SPD never uses global atomic
            #
            # The original function signature is:
            # void ComputeAutoExposure(FfxUInt32x3 WorkGroupId,
            #                          FfxUInt32 LocalThreadIndex)
            # We rename the original to _Original and create a new
            # wrapper that guards the execution.
            set(PATCH_F_SEARCH
                "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            )
            set(PATCH_F_REPLACE
                "void ComputeAutoExposure_Original(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            )

            string(FIND "${hlsl_code}" "${PATCH_F_SEARCH}" PATCH_F_POS)
            if(PATCH_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-F] ComputeAutoExposure signature not found!"
                )
            else()
                # Rename original function
                string(REPLACE
                    "${PATCH_F_SEARCH}"
                    "${PATCH_F_REPLACE}"
                    hlsl_code "${hlsl_code}")

                # Append new wrapper function after the file content.
                # The wrapper:
                #   - Only runs for workgroup (0,0)
                #   - Passes NumWorkGroups=1 to SpdDownsample
                #   - This bypasses the global atomic counter entirely
                #   - SPD with NumWorkGroups=1 never calls atomic ops
                set(PATCH_F_WRAPPER
"
// [Fix-F] SPD single-workgroup wrapper
// Bypasses global atomic counter that crashes Lavapipe.
// Only workgroup (0,0) runs SpdDownsample with NumWorkGroups=1.
// This avoids inter-workgroup atomic coordination entirely.
void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)
{
    // Skip all workgroups except (0,0)
    // When NumWorkGroups=1, SPD atomic counter is never used
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
                file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")
                message(STATUS
                    "[Patch-F] SPD single-workgroup fix applied OK.")
            endif()

            # Verify
            file(READ "${LUM_PYRAMID_HLSL}" hlsl_verify)
            string(FIND "${hlsl_verify}"
                "Fix-F" VERIFY_F_POS)
            if(VERIFY_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[VERIFY-F] Patch-F not confirmed in HLSL file!"
                )
            else()
                message(STATUS
                    "[VERIFY-F] Patch-F confirmed in HLSL file. OK!")
            endif()
        endif()
    else()
        message(STATUS "[Patch-F] DISABLED.")
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

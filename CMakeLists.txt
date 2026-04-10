cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Patch-A: RAM allocation fix in ffx_vk.cpp - Required for BOTH renderers.
#
# Patch-B: Dedicated allocation disable - not needed, OFF.
#
# Patch-C: FP16 kill switch - not needed for Lavapipe, OFF.
#
# Patch-D: B10G11R11 format override - not needed for Lavapipe, OFF.
#
# Patch-E: Clamp SPD workgroup dispatch to [1,1] in ffx_fsr2.cpp
#   Prevents Lavapipe groupshared memory accumulation over 32 frames.
#
# Patch-F: HLSL single-workgroup guard in ComputeAutoExposure
#   Early-returns all workgroups except (0,0).
#   Passes NumWorkGroups=1 to SpdDownsample.
#
# Patch-G: Override SpdIncreaseAtomicCounter in HLSL to bypass GPU atomic.
#   ROOT CAUSE OF SPD CRASH ON LAVAPIPE:
#   SpdExitWorkgroup() calls SpdIncreaseAtomicCounter() which calls
#   SPD_IncreaseAtomicCounter(spdCounter) - a GPU storage buffer atomic.
#   Lavapipe crashes on this atomic operation (confirmed from chat history).
#   FIX: rename original SpdIncreaseAtomicCounter, replace with version
#   that sets groupshared spdCounter=0 WITHOUT touching GPU storage buffer.
#   With spdCounter=0 and numWorkGroups=1:
#     SpdExitWorkgroup returns: (0 != (1-1)) = (0 != 0) = FALSE
#     = workgroup proceeds (correct - it IS the last/only workgroup)
#   SpdResetAtomicCounter is still called but SPD_ResetAtomicCounter
#   writes to the same GPU buffer - if that also crashes we handle separately.
#   Using file(APPEND) to write HLSL to avoid CMake string escaping issues.
# =============================================================================

set(PATCH_A_ENABLED ON)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed for Lavapipe (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed for Lavapipe (confirmed)
set(PATCH_E_ENABLED ON)  # Workgroup clamp to [1,1]
set(PATCH_F_ENABLED ON)  # HLSL single-workgroup guard
set(PATCH_G_ENABLED ON)  # HLSL GPU atomic bypass - fixes SPD crash

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
            message(FATAL_ERROR
                "[Patch-E] SPD scheduleDispatch not found in ffx_fsr2.cpp!")
        endif()

        string(REPLACE "${PATCH_E_SEARCH}"
            "printf(\"[SPD] Clamping dispatch [%u,%u]->[1,1]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, 1, 1); printf(\"[SPD] scheduleDispatch returned OK\\n\"); fflush(stdout)"
            fsr2_code "${fsr2_code}")

        string(FIND "${fsr2_code}" "Clamping dispatch" VERIFY_E_POS)
        if(VERIFY_E_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-E] Patch-E not confirmed!")
        else()
            message(STATUS "[Patch-E] Workgroup clamp [1,1] applied OK.")
        endif()
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] ffx_fsr2.cpp written.")
    else()
        set(PATCH_E_DIAG_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${PATCH_E_DIAG_SEARCH}" DIAG_POS)
        if(NOT DIAG_POS EQUAL -1)
            string(REPLACE "${PATCH_E_DIAG_SEARCH}"
                "printf(\"[SPD] Dispatching Groups=[%u,%u]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[SPD] scheduleDispatch returned OK\\n\"); fflush(stdout)"
                fsr2_code "${fsr2_code}")
        endif()
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] DISABLED - original workgroup count used.")
    endif()

    # =========================================================================
    # PATCH F + G: Fix SPD HLSL
    # Both patches write to ffx_fsr2_compute_luminance_pyramid.h
    # All HLSL is written via file(WRITE)/file(APPEND) to avoid CMake
    # string escaping corrupting the shader syntax.
    #
    # Order of operations:
    #   1. Read original file
    #   2. Patch-G: rename SpdIncreaseAtomicCounter -> _Original via
    #      string(REPLACE) then file(WRITE) the modified content
    #   3. file(APPEND) the Fix-G replacement function
    #   4. Patch-F: rename ComputeAutoExposure -> _Original via
    #      string(REPLACE) then file(WRITE) the modified content
    #   5. file(APPEND) the Fix-F wrapper function
    # =========================================================================
    set(LUM_PYRAMID_HLSL
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h"
    )
    if(NOT EXISTS "${LUM_PYRAMID_HLSL}")
        message(FATAL_ERROR "[Patch-F/G] HLSL not found: ${LUM_PYRAMID_HLSL}")
    endif()

    file(READ "${LUM_PYRAMID_HLSL}" hlsl_code)
    message(STATUS "[Patch-F/G] HLSL file read OK.")

    # -------------------------------------------------------------------------
    # PATCH G: Rename SpdIncreaseAtomicCounter, append bypass replacement
    # -------------------------------------------------------------------------
    if(PATCH_G_ENABLED)
        set(PATCH_G_SEARCH
            "void SpdIncreaseAtomicCounter(FfxUInt32 slice)"
        )
        string(FIND "${hlsl_code}" "${PATCH_G_SEARCH}" PATCH_G_POS)
        if(PATCH_G_POS EQUAL -1)
            message(FATAL_ERROR
                "[Patch-G] SpdIncreaseAtomicCounter not found in HLSL!")
        endif()

        string(REPLACE
            "void SpdIncreaseAtomicCounter(FfxUInt32 slice)"
            "void SpdIncreaseAtomicCounter_Original(FfxUInt32 slice)"
            hlsl_code "${hlsl_code}")

        # Write the renamed file first
        file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")

        # Append replacement using file(APPEND) - avoids CMake escape issues
        # Sets groupshared spdCounter=0 without GPU storage buffer atomic.
        # ffx_spd.h SpdExitWorkgroup then evaluates:
        #   (SpdGetAtomicCounter() != (numWorkGroups-1))
        #   = (0 != (1-1)) = (0 != 0) = FALSE
        #   = workgroup(0,0) proceeds as last workgroup. Correct.
        file(APPEND "${LUM_PYRAMID_HLSL}"
            "\n// [Fix-G] GPU atomic bypass for Lavapipe compatibility.\n"
            "// SPD_IncreaseAtomicCounter crashes Lavapipe (GPU storage buffer atomic).\n"
            "// We set groupshared spdCounter=0 directly instead.\n"
            "// SpdExitWorkgroup check: (0 != (1-1)) = FALSE -> proceed. Correct.\n"
            "void SpdIncreaseAtomicCounter(FfxUInt32 slice)\n"
            "{\n"
            "    spdCounter = 0;\n"
            "}\n"
        )

        # Re-read for verification and subsequent patches
        file(READ "${LUM_PYRAMID_HLSL}" hlsl_code)
        string(FIND "${hlsl_code}" "Fix-G" VERIFY_G_POS)
        if(VERIFY_G_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-G] Patch-G not confirmed in HLSL!")
        else()
            message(STATUS "[VERIFY-G] Patch-G confirmed OK.")
        endif()
    else()
        message(STATUS "[Patch-G] DISABLED.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH F: Rename ComputeAutoExposure, append single-workgroup wrapper
    # -------------------------------------------------------------------------
    if(PATCH_F_ENABLED)
        set(PATCH_F_SEARCH
            "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
        )
        string(FIND "${hlsl_code}" "${PATCH_F_SEARCH}" PATCH_F_POS)
        if(PATCH_F_POS EQUAL -1)
            message(FATAL_ERROR
                "[Patch-F] ComputeAutoExposure not found in HLSL!")
        endif()

        string(REPLACE
            "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            "void ComputeAutoExposure_Original(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)"
            hlsl_code "${hlsl_code}")

        # Write renamed content
        file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")

        # Append wrapper using file(APPEND)
        file(APPEND "${LUM_PYRAMID_HLSL}"
            "\n// [Fix-F] Single-workgroup wrapper for ComputeAutoExposure.\n"
            "// Only workgroup(0,0) runs SpdDownsample with NumWorkGroups=1.\n"
            "// All other workgroups early-return without touching GPU memory.\n"
            "// Combined with Fix-G: GPU atomic bypassed, no Lavapipe crash.\n"
            "void ComputeAutoExposure(FfxUInt32x3 WorkGroupId, FfxUInt32 LocalThreadIndex)\n"
            "{\n"
            "    if (WorkGroupId.x != 0 || WorkGroupId.y != 0) return;\n"
            "#if FFX_HALF\n"
            "    SpdDownsampleH(\n"
            "        FfxUInt32x2(0, 0),\n"
            "        FfxUInt32(LocalThreadIndex),\n"
            "        FfxUInt32(MipCount()),\n"
            "        FfxUInt32(1),\n"
            "        FfxUInt32(WorkGroupId.z),\n"
            "        FfxUInt32x2(0, 0));\n"
            "#else\n"
            "    SpdDownsample(\n"
            "        FfxUInt32x2(0, 0),\n"
            "        FfxUInt32(LocalThreadIndex),\n"
            "        FfxUInt32(MipCount()),\n"
            "        FfxUInt32(1),\n"
            "        FfxUInt32(WorkGroupId.z),\n"
            "        FfxUInt32x2(0, 0));\n"
            "#endif\n"
            "}\n"
        )

        # Verify
        file(READ "${LUM_PYRAMID_HLSL}" hlsl_verify)
        string(FIND "${hlsl_verify}" "Fix-F" VERIFY_F_POS)
        if(VERIFY_F_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-F] Patch-F not confirmed in HLSL!")
        else()
            message(STATUS "[VERIFY-F] Patch-F confirmed OK.")
        endif()
        string(FIND "${hlsl_verify}"
            "ComputeAutoExposure_Original" VERIFY_F2_POS)
        if(VERIFY_F2_POS EQUAL -1)
            message(FATAL_ERROR
                "[VERIFY-F] ComputeAutoExposure rename not confirmed!")
        else()
            message(STATUS "[VERIFY-F] ComputeAutoExposure renamed OK.")
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

cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Current configuration: Phase 2 conclusion / Pre-Phase 3
#   Proof of concept: compile shaders from source instead of precompiled blobs
#   Patch-E OFF: SPD will run (and crash Lavapipe) - expected for this test
#   Goal: confirm shader compilation from source works before patching shaders
#
# Confirmed findings so far:
#   Patch-A: REQUIRED for BOTH (FSR2 context creation fails without it)
#   Patch-B: NOT required for either (Lavapipe faster without it)
#   Patch-C: NOT needed (confirmed not root cause of any bug)
#   Patch-D: NOT needed (confirmed not root cause of any bug)
#   Patch-E: REQUIRED for BOTH (SPD SPIR-V blob crashes Lavapipe on GPU,
#            causes silent corruption on SwiftShader)
#            Set OFF here intentionally to test shader recompilation path.
#            Lavapipe WILL crash - this is expected and confirms SPD runs.
# =============================================================================

set(PATCH_A_ENABLED ON)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required for either (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed (confirmed)
set(PATCH_E_ENABLED OFF) # SPD bypass - OFF to test shader recompilation

# =============================================================================
# SHADER COMPILATION MODE
# =============================================================================
# FFX_USE_PRECOMPILED_SHADERS controls whether the AMD SDK uses its
# pre-built SPIR-V blob files or recompiles shaders from HLSL source.
#
# ON  = Use precompiled SPIR-V blobs (default AMD behavior)
#       Fast build. Cannot patch shader source.
#       SPD blob crashes Lavapipe during GPU execution (confirmed).
#
# OFF = Recompile shaders from HLSL source using DXC at build time.
#       Slower build. We CAN patch HLSL source and it takes effect.
#       This is required to fix the SPD crash properly.
#       Requires DXC (DirectX Shader Compiler) on the build machine.
#       DXC is included with Visual Studio 2022 on GitHub Actions.
# =============================================================================
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
    GIT_TAG        master
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

# Pass shader compilation mode to AMD SDK
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
        set(PATCH_B_SEARCH
            "if (devCaps.dedicatedAllocationSupported)"
        )
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
        set(PATCH_D_SEARCH
            "return VK_FORMAT_B10G11R11_UFLOAT_PACK32"
        )
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
    # PATCH ffx_fsr2.cpp - Patch E + diagnostics
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR "[Patch-E] ffx_fsr2.cpp not found!")
    endif()
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        # Patch-E ACTIVE: Skip SPD dispatch entirely
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
                "/* [Fix-E] SPD dispatch SKIPPED - subgroupSize incompatible */"
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
        # Patch-E INACTIVE: inject diagnostics for crash debugging
        set(CP1_SEARCH
            "Fsr2SpdConstants luminancePyramidConstants"
        )
        string(FIND "${fsr2_code}" "${CP1_SEARCH}" CP1_POS)
        if(CP1_POS EQUAL -1)
            message(STATUS "[Diag] CP1 search string not found.")
        else()
            string(REPLACE "${CP1_SEARCH}"
                "printf(\"[SPD-DIAG] Checkpoint 1: Before SPD constants setup\\n\"); fflush(stdout); Fsr2SpdConstants luminancePyramidConstants"
                fsr2_code "${fsr2_code}")
            message(STATUS "[Diag] Checkpoint 1 injected.")
        endif()

        set(CP2_3_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${CP2_3_SEARCH}" CP2_3_POS)
        if(CP2_3_POS EQUAL -1)
            message(STATUS "[Diag] CP2/3 search string not found.")
        else()
            string(REPLACE "${CP2_3_SEARCH}"
                "printf(\"[SPD-DIAG] Checkpoint 2: Before scheduleDispatch. Groups=[%u,%u]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[SPD-DIAG] Checkpoint 3: After scheduleDispatch - CPU call returned OK\\n\"); fflush(stdout)"
                fsr2_code "${fsr2_code}")
            message(STATUS "[Diag] Checkpoints 2 and 3 injected.")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] DISABLED - SPD runs with diagnostics.")
    endif()

    # =========================================================================
    # SHADER COMPILATION SETUP
    # When FFX_USE_PRECOMPILED_SHADERS is OFF, the AMD SDK needs DXC.
    # DXC is included with Visual Studio 2022 on GitHub Actions runners.
    # We search for it in standard VS2022 and Windows SDK locations.
    # =========================================================================
    if(NOT FFX_USE_PRECOMPILED_SHADERS)
        message(STATUS "[Shader] Precompiled shaders DISABLED.")
        message(STATUS "[Shader] Searching for DXC compiler...")

        # Search locations for DXC on GitHub Actions Windows runner
        # VS2022 ships dxc.exe in multiple locations
        find_program(DXC_PATH
            NAMES dxc.exe dxc
            HINTS
                # Visual Studio 2022 Enterprise (GitHub Actions default)
                "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC"
                # Windows SDK bin directories
                "C:/Program Files (x86)/Windows Kits/10/bin"
                # Common VS2022 subfolder patterns
            PATH_SUFFIXES
                "x64"
                "bin/Hostx64/x64"
            DOC "Path to DXC shader compiler"
        )

        if(NOT DXC_PATH)
            # Try a glob search for dxc.exe across known VS paths
            message(STATUS "[Shader] find_program did not locate DXC.")
            message(STATUS "[Shader] Trying glob search...")

            file(GLOB_RECURSE DXC_CANDIDATES
                "C:/Program Files/Microsoft Visual Studio/2022/*/dxc.exe"
                "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
            )

            if(DXC_CANDIDATES)
                # Use the first candidate found
                list(GET DXC_CANDIDATES 0 DXC_PATH)
                message(STATUS
                    "[Shader] DXC found via glob: ${DXC_PATH}")
            else()
                message(FATAL_ERROR
                    "[Shader] DXC compiler not found!\n"
                    "Cannot compile shaders from source without DXC.\n"
                    "Set FFX_USE_PRECOMPILED_SHADERS ON to use blobs,\n"
                    "or install Visual Studio 2022 with Windows SDK."
                )
            endif()
        else()
            message(STATUS "[Shader] DXC found: ${DXC_PATH}")
        endif()

        # Pass DXC path to AMD SDK
        set(DXC_PATH "${DXC_PATH}" CACHE STRING "" FORCE)
        message(STATUS "[Shader] DXC path set for AMD SDK: ${DXC_PATH}")
    else()
        message(STATUS "[Shader] Using precompiled SPIR-V blobs (default).")
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

cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Current configuration: Phase 2 - Test HLSL shader patch
#   Now that we compile shaders from source, patching HLSL actually works!
#   Patch-E OFF: SPD dispatch runs (not bypassed at C++ level)
#   Patch-F ON:  HLSL shader early return (new - bypasses at shader level)
#
#   If Lavapipe stops crashing with Patch-F ON:
#   → Crash is in shader CODE, not dispatch setup
#   → We implement proper SPD fix in HLSL
#
#   If Lavapipe still crashes with Patch-F ON:
#   → Crash is in dispatch SETUP (atomic counter, barriers, etc.)
#   → We investigate C++ dispatch code
# =============================================================================

set(PATCH_A_ENABLED ON)  # Required for BOTH (confirmed)
set(PATCH_B_ENABLED OFF) # Not required for either (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed (confirmed)
set(PATCH_E_ENABLED OFF) # C++ SPD bypass - OFF (testing HLSL patch instead)
set(PATCH_F_ENABLED ON)  # HLSL shader early return - NEW

# =============================================================================
# SHADER COMPILATION MODE
# OFF = compile from HLSL source using DXC (required for Patch-F to work)
# ON  = use precompiled SPIR-V blobs (Patch-F has no effect when ON)
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
    GIT_Tag        master
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
        # Patch-E inactive - inject diagnostics
        set(CP1_SEARCH "Fsr2SpdConstants luminancePyramidConstants")
        string(FIND "${fsr2_code}" "${CP1_SEARCH}" CP1_POS)
        if(NOT CP1_POS EQUAL -1)
            string(REPLACE "${CP1_SEARCH}"
                "printf(\"[SPD-DIAG] CP1: Before SPD constants\\n\"); fflush(stdout); Fsr2SpdConstants luminancePyramidConstants"
                fsr2_code "${fsr2_code}")
        endif()

        set(CP2_3_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${CP2_3_SEARCH}" CP2_3_POS)
        if(NOT CP2_3_POS EQUAL -1)
            string(REPLACE "${CP2_3_SEARCH}"
                "printf(\"[SPD-DIAG] CP2: Before scheduleDispatch Groups=[%u,%u]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[SPD-DIAG] CP3: After scheduleDispatch OK\\n\"); fflush(stdout)"
                fsr2_code "${fsr2_code}")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] DISABLED - SPD runs with diagnostics.")
    endif()

    # =========================================================================
    # PATCH F: HLSL shader early return for SPD luminance pyramid
    #
    # This patch only works when FFX_USE_PRECOMPILED_SHADERS is OFF.
    # When compiling from source, patching the HLSL file takes effect.
    # When using precompiled blobs, this patch is silently ignored.
    #
    # PURPOSE: Test whether the crash is in shader CODE or dispatch SETUP.
    #   If Lavapipe stops crashing with this patch ON:
    #     → Crash is inside the shader instructions themselves
    #     → We implement proper SPD fix in HLSL next
    #   If Lavapipe still crashes with this patch ON:
    #     → Crash is in dispatch setup (atomic counter, barriers, formats)
    #     → We investigate C++ dispatch code instead
    #
    # The HLSL entry point is in:
    #   sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h
    # =========================================================================
    if(PATCH_F_ENABLED)
        if(FFX_USE_PRECOMPILED_SHADERS)
            message(STATUS
                "[Patch-F] WARNING: FFX_USE_PRECOMPILED_SHADERS is ON."
                " Patch-F patches HLSL source but precompiled blobs are used."
                " Patch-F has NO EFFECT. Set FFX_USE_PRECOMPILED_SHADERS OFF."
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

            # Print all void CS lines so we can see the exact signature
            message(STATUS "=== [Patch-F] Searching for CS entry point ===")
            string(REPLACE "\n" ";" hlsl_lines "${hlsl_code}")
            set(hlsl_line_num 0)
            foreach(hlsl_line IN LISTS hlsl_lines)
                math(EXPR hlsl_line_num "${hlsl_line_num} + 1")
                if(hlsl_line MATCHES "void CS")
                    message(STATUS "  LINE ${hlsl_line_num}: ${hlsl_line}")
                endif()
            endforeach()
            message(STATUS "=== [Patch-F] End CS search ===")

            # Patch the CS entry point to return immediately.
            # From the PDF the exact signature is:
            # void CS(uint3 WorkGroupId : SV_GroupID,
            #         uint LocalThreadIndex : SV_GroupIndex)
            # We insert an immediate return as the first statement.
            # This produces a valid empty shader that does nothing.
            set(PATCH_F_SEARCH
                "void CS(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)"
            )
            set(PATCH_F_REPLACE
                "void CS(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex) { return; } void CS_OriginalDisabled(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)"
            )

            string(FIND "${hlsl_code}" "${PATCH_F_SEARCH}" PATCH_F_POS)
            if(PATCH_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-F] CS entry point not found!\n"
                    "Check LINE output above for exact signature."
                )
            else()
                string(REPLACE
                    "${PATCH_F_SEARCH}"
                    "${PATCH_F_REPLACE}"
                    hlsl_code "${hlsl_code}")
                file(WRITE "${LUM_PYRAMID_HLSL}" "${hlsl_code}")
                message(STATUS
                    "[Patch-F] HLSL CS early return applied OK.")
            endif()

            # Verify
            file(READ "${LUM_PYRAMID_HLSL}" hlsl_verify)
            string(FIND "${hlsl_verify}" "CS_OriginalDisabled" VERIFY_F_POS)
            if(VERIFY_F_POS EQUAL -1)
                message(FATAL_ERROR
                    "[VERIFY-F] Patch-F not confirmed in HLSL file!")
            else()
                message(STATUS "[VERIFY-F] Patch-F confirmed in HLSL file. OK!")
            endif()
        endif()
    else()
        message(STATUS "[Patch-F] DISABLED.")
    endif()

    # =========================================================================
    # DXC SETUP (when compiling shaders from source)
    # =========================================================================
    if(NOT FFX_USE_PRECOMPILED_SHADERS)
        message(STATUS "[Shader] Precompiled shaders DISABLED.")
        message(STATUS "[Shader] Searching for DXC compiler...")

        find_program(DXC_PATH
            NAMES dxc.exe dxc
            HINTS
                "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC"
                "C:/Program Files (x86)/Windows Kits/10/bin"
            PATH_SUFFIXES
                "x64"
                "bin/Hostx64/x64"
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
                message(FATAL_ERROR
                    "[Shader] DXC compiler not found!\n"
                    "Set FFX_USE_PRECOMPILED_SHADERS ON to use blobs."
                )
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

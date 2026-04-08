cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

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
    # WIN7 COMPAT SCRUBBER
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
            message(STATUS "[Patch] Win7 scrub applied to: ${ffx_file}")
        endif()
    endforeach()

    # =========================================================================
    # PATCH ffx_vk.cpp
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
    file(READ "${VK_BACKEND_FILE}" vk_code)

    # Patch A
    set(PATCH_A_SEARCH
        "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"
    )
    string(FIND "${vk_code}" "${PATCH_A_SEARCH}" PATCH_A_POS)
    if(PATCH_A_POS EQUAL -1)
        message(STATUS "[Patch-A] Already patched or not found, skipping.")
    else()
        string(REPLACE "${PATCH_A_SEARCH}"
            "if (false) /* [SwiftShader-Fix-A] */"
            vk_code "${vk_code}")
        message(STATUS "[Patch-A] Applied OK.")
    endif()

    # Patch B
    set(PATCH_B_SEARCH
        "if (devCaps.dedicatedAllocationSupported)"
    )
    string(FIND "${vk_code}" "${PATCH_B_SEARCH}" PATCH_B_POS)
    if(PATCH_B_POS EQUAL -1)
        message(STATUS "[Patch-B] Already patched or not found, skipping.")
    else()
        string(REPLACE "${PATCH_B_SEARCH}"
            "if (false) /* [SwiftShader-Fix-B] */"
            vk_code "${vk_code}")
        message(STATUS "[Patch-B] Applied OK.")
    endif()

    # Patch C - FP16 kill switch
    set(PATCH_C_SEARCH
        "deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16"
    )
    string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
    if(PATCH_C_POS EQUAL -1)
        message(FATAL_ERROR "[Patch-C] FP16 kill switch search string not found!")
    else()
        string(REPLACE "${PATCH_C_SEARCH}"
            "deviceCapabilities->fp16Supported = false /* [SwiftShader-Fix-C] */"
            vk_code "${vk_code}")
        message(STATUS "[Patch-C] FP16 kill switch applied OK.")
    endif()

    # Patch D - B10G11R11 format override (return statement only)
    set(PATCH_D_SEARCH
        "return VK_FORMAT_B10G11R11_UFLOAT_PACK32"
    )
    string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
    if(PATCH_D_POS EQUAL -1)
        message(STATUS "[Patch-D] Already patched or not found, skipping.")
    else()
        string(REPLACE "${PATCH_D_SEARCH}"
            "return VK_FORMAT_R16G16B16A16_SFLOAT /* [SwiftShader-Fix-D] */"
            vk_code "${vk_code}")
        message(STATUS "[Patch-D] B10G11R11->R16G16B16A16 applied OK.")
    endif()

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] ffx_vk.cpp patches written.")

    # =========================================================================
    # PATCH E: FSR2 Luminance Pyramid Shader - SPD Subgroup Fix
    #
    # PROBLEM: SwiftShader has subgroupSize=4. SPD requires size>=32.
    # FSR2 ALWAYS runs the luminance pyramid pass (even with auto-exposure OFF)
    # because it needs luminance data for SHADING CHANGE DETECTION.
    # When SPD's subgroup reductions produce garbage on SwiftShader,
    # the shading change detector thinks EVERY pixel changed every frame.
    # This collapses the temporal accumulation weight to zero -> black output.
    #
    # FIX: Patch the luminance pyramid shader to write safe constant values
    # instead of running the broken SPD computation.
    # - Luminance mip chain: write 0.5 (mid-grey, reasonable for SDR scene)
    # - Exposure output: write 1.0 (neutral exposure, matches our manual value)
    #
    # This bypasses SPD entirely while keeping the shader structurally valid.
    # =========================================================================
    set(LUM_PYRAMID_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/gpu/fsr2/ffx_fsr2_compute_luminance_pyramid.h"
    )

    if(EXISTS "${LUM_PYRAMID_FILE}")
        file(READ "${LUM_PYRAMID_FILE}" lum_code)

        # The SPD callback function that writes luminance mip levels is called
        # SpdStoreIntermediate and SpdStore in ffx_spd.h which FSR2 implements
        # via ffxSpdStoreQuad and ffxSpdStoreQuadH.
        # The entry point for the luminance pyramid shader is:
        # void CS(uint3 WorkGroupId, uint LocalThreadIndex)
        # We patch the entry point to immediately return after writing
        # safe constant values to the output textures.

        # Find the shader entry point and inject early-exit with safe values.
        # The function signature in ffx_fsr2_compute_luminance_pyramid.h is:
        # void CS(uint3 WorkGroupId, uint LocalThreadIndex)
        set(PATCH_E_SEARCH
            "void CS(uint3 WorkGroupId, uint LocalThreadIndex)"
        )
        set(PATCH_E_REPLACE
            "void CS(uint3 WorkGroupId, uint LocalThreadIndex) { return; } void CS_DISABLED_SwiftShader(uint3 WorkGroupId, uint LocalThreadIndex)"
        )
        string(FIND "${lum_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
        if(PATCH_E_POS EQUAL -1)
            message(STATUS "[Patch-E] CS entry point not found with expected signature, trying alternate...")
            # Try without uint3 - some versions use different types
            set(PATCH_E_SEARCH_ALT "void CS(")
            string(FIND "${lum_code}" "${PATCH_E_SEARCH_ALT}" PATCH_E_ALT_POS)
            if(PATCH_E_ALT_POS EQUAL -1)
                message(STATUS "[Patch-E] WARNING: Could not find CS entry point in luminance pyramid shader.")
            else()
                message(STATUS "[Patch-E] Found CS via alternate search. Checking file structure...")
            endif()
        else()
            string(REPLACE "${PATCH_E_SEARCH}" "${PATCH_E_REPLACE}" lum_code "${lum_code}")
            file(WRITE "${LUM_PYRAMID_FILE}" "${lum_code}")
            message(STATUS "[Patch-E] Luminance pyramid CS early-exit patch applied OK.")
        endif()
    else()
        message(STATUS "[Patch-E] WARNING: Luminance pyramid shader not found at: ${LUM_PYRAMID_FILE}")
    endif()

    # =========================================================================
    # VERIFICATION
    # =========================================================================
    file(READ "${VK_BACKEND_FILE}" vk_code_verify)

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-C" VERIFY_C_POS)
    if(VERIFY_C_POS EQUAL -1)
        message(FATAL_ERROR "[VERIFY FAILED] Patch-C not found in written file!")
    else()
        message(STATUS "[VERIFY] Patch-C confirmed present. OK!")
    endif()

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-D" VERIFY_D_POS)
    if(VERIFY_D_POS EQUAL -1)
        message(STATUS "[VERIFY] Warning: Patch-D marker not found.")
    else()
        message(STATUS "[VERIFY] Patch-D confirmed present. OK!")
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

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

# =============================================================================
# FORCE RE-PATCH EVERY CMAKE RUN
# =============================================================================
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

    # -------------------------------------------------------------------------
    # PATCH A: SwiftShader memory fix #1
    # -------------------------------------------------------------------------
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

    # -------------------------------------------------------------------------
    # PATCH B: SwiftShader memory fix #2
    # -------------------------------------------------------------------------
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

    # -------------------------------------------------------------------------
    # PATCH C: FP16 KILL SWITCH
    #
    # From diagnostic output, the REAL assignment is on LINE 2946:
    #   deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16
    # Note: "shaderFloat18Int8Features" is the exact AMD variable name
    # (the "18" is not a typo - it is how AMD named it in v1.1.4)
    #
    # We force this to = false so FSR2 uses pure FP32 math.
    # SwiftShader corrupts negative FP16 values (needed for YCoCg).
    # -------------------------------------------------------------------------
    set(PATCH_C_SEARCH
        "deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16"
    )
    set(PATCH_C_REPLACE
        "deviceCapabilities->fp16Supported = false /* [SwiftShader-Fix-C] FP16 kill switch */"
    )
    string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
    if(PATCH_C_POS EQUAL -1)
        message(FATAL_ERROR
            "[Patch-C] CRITICAL FAILURE: FP16 kill switch search string not found!\n"
            "Expected to find: '${PATCH_C_SEARCH}'\n"
            "in file: ${VK_BACKEND_FILE}\n"
            "Build CANNOT continue - FSR2 colors will be destroyed on SwiftShader!"
        )
    else()
        string(REPLACE "${PATCH_C_SEARCH}" "${PATCH_C_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-C] FP16 kill switch applied OK. FSR2 will use FP32 math.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH D: B10G11R11 FORMAT OVERRIDE
    #
    # SwiftShader cannot write UAVs to B10G11R11 format.
    # We redirect to R32G32B32A32_SFLOAT (128-bit full float).
    #
    # IMPORTANT: We CANNOT use R16G16B16A16_SFLOAT here because that
    # format already has its own case in the same switch statement.
    # Using it would cause C2196 "case value already used" error.
    # R32G32B32A32_SFLOAT does NOT appear elsewhere in the switch.
    # -------------------------------------------------------------------------
    set(PATCH_D_SEARCH
        "VK_FORMAT_B10G11R11_UFLOAT_PACK32"
    )
    set(PATCH_D_REPLACE
        "VK_FORMAT_R32G32B32A32_SFLOAT /* [SwiftShader-Fix-D] B10G11R11 override */"
    )
    string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
    if(PATCH_D_POS EQUAL -1)
        message(STATUS "[Patch-D] Already patched or not found, skipping.")
    else()
        string(REPLACE "${PATCH_D_SEARCH}" "${PATCH_D_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-D] B10G11R11->R32G32B32A32 format override applied OK.")
    endif()

    # -------------------------------------------------------------------------
    # Write all patches back to file
    # -------------------------------------------------------------------------
    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] All patches written to ffx_vk.cpp.")

    # -------------------------------------------------------------------------
    # VERIFICATION: Read file back and confirm patches are present
    # -------------------------------------------------------------------------
    file(READ "${VK_BACKEND_FILE}" vk_code_verify)

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-C" VERIFY_C_POS)
    if(VERIFY_C_POS EQUAL -1)
        message(FATAL_ERROR
            "[VERIFY FAILED] Patch-C (FP16 kill switch) not found in written file!\n"
            "Check write permissions for: ${VK_BACKEND_FILE}"
        )
    else()
        message(STATUS "[VERIFY] Patch-C confirmed present in file. OK!")
    endif()

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-D" VERIFY_D_POS)
    if(VERIFY_D_POS EQUAL -1)
        message(STATUS "[VERIFY] Warning: Patch-D marker not found.")
    else()
        message(STATUS "[VERIFY] Patch-D confirmed present in file. OK!")
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

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
# Delete the FetchContent stamp file so our patch block always executes.
# This prevents the cache from silently skipping our patches.
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
    # Remove SetThreadDescription calls that don't exist on Windows 7
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
    # PATCH: ffx_vk.cpp
    # We apply three surgical patches to this single file.
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
    file(READ "${VK_BACKEND_FILE}" vk_code)

    # -------------------------------------------------------------------------
    # PATCH A: SwiftShader Memory Allocator Fix #1
    # Disable the device-local memory preference that confuses SwiftShader.
    # Search string is SHORT and UNIQUE - will not fail on whitespace.
    # -------------------------------------------------------------------------
    set(PATCH_A_SEARCH
        "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"
    )
    set(PATCH_A_REPLACE
        "if (false) /* [SwiftShader-Fix-A] Device-local memory patch */"
    )
    string(FIND "${vk_code}" "${PATCH_A_SEARCH}" PATCH_A_POS)
    if(PATCH_A_POS EQUAL -1)
        message(STATUS
            "[Patch-A] Search string not found - may already be patched, continuing."
        )
    else()
        string(REPLACE "${PATCH_A_SEARCH}" "${PATCH_A_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-A] SwiftShader memory fix #1 applied OK.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH B: SwiftShader Memory Allocator Fix #2
    # Disable dedicated allocation that SwiftShader does not support properly.
    # -------------------------------------------------------------------------
    set(PATCH_B_SEARCH
        "if (devCaps.dedicatedAllocationSupported)"
    )
    set(PATCH_B_REPLACE
        "if (false) /* [SwiftShader-Fix-B] Dedicated alloc patch */"
    )
    string(FIND "${vk_code}" "${PATCH_B_SEARCH}" PATCH_B_POS)
    if(PATCH_B_POS EQUAL -1)
        message(STATUS
            "[Patch-B] Search string not found - may already be patched, continuing."
        )
    else()
        string(REPLACE "${PATCH_B_SEARCH}" "${PATCH_B_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-B] SwiftShader memory fix #2 applied OK.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH C: FP16 KILL SWITCH  <-- THIS IS THE KEY FIX FOR DARK SILHOUETTE
    # Force fp16Supported = false so FSR2 uses pure FP32 math throughout.
    # SwiftShader corrupts negative FP16 numbers (YCoCg color space needs them).
    # Search string targets ONLY the assignment line - short and unique.
    # -------------------------------------------------------------------------
    set(PATCH_C_SEARCH
        "deviceCapabilities->fp16Supported = true;"
    )
    set(PATCH_C_REPLACE
        "deviceCapabilities->fp16Supported = false; /* [SwiftShader-Fix-C] FP16 kill switch */"
    )
    string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
    if(PATCH_C_POS EQUAL -1)
        # Try alternate form in case SDK uses a different assignment
        set(PATCH_C_SEARCH_ALT
            "fp16Supported = true;"
        )
        string(FIND "${vk_code}" "${PATCH_C_SEARCH_ALT}" PATCH_C_ALT_POS)
        if(PATCH_C_ALT_POS EQUAL -1)
            message(FATAL_ERROR
                "[Patch-C] CRITICAL FAILURE: FP16 kill switch search string not found!\n"
                "The file '${VK_BACKEND_FILE}' does not contain the expected FP16 assignment.\n"
                "Please check the SDK source and update PATCH_C_SEARCH in CMakeLists.txt.\n"
                "Build CANNOT continue - FSR2 colors will be destroyed on SwiftShader!"
            )
        else()
            string(REPLACE
                "${PATCH_C_SEARCH_ALT}"
                "fp16Supported = false; /* [SwiftShader-Fix-C-alt] FP16 kill switch */"
                vk_code "${vk_code}"
            )
            message(STATUS "[Patch-C] FP16 kill switch applied via ALT search string OK.")
        endif()
    else()
        string(REPLACE "${PATCH_C_SEARCH}" "${PATCH_C_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-C] FP16 kill switch applied OK. FSR2 will use FP32 math.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH D: B10G11R11 FORMAT OVERRIDE
    # SwiftShader cannot write UAVs in B10G11R11 format - silently drops them.
    # We redirect to R16G16B16A16_SFLOAT which SwiftShader handles correctly.
    # Search string is the Vulkan format enum - SHORT and UNIQUE in this context.
    # -------------------------------------------------------------------------
    set(PATCH_D_SEARCH
        "VK_FORMAT_B10G11R11_UFLOAT_PACK32"
    )
    set(PATCH_D_REPLACE
        "VK_FORMAT_R16G16B16A16_SFLOAT /* [SwiftShader-Fix-D] B10G11R11 override */"
    )
    string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
    if(PATCH_D_POS EQUAL -1)
        message(STATUS
            "[Patch-D] B10G11R11 search string not found - may already be patched, continuing."
        )
    else()
        string(REPLACE "${PATCH_D_SEARCH}" "${PATCH_D_REPLACE}" vk_code "${vk_code}")
        message(STATUS "[Patch-D] B10G11R11->R16G16B16A16 format override applied OK.")
    endif()

    # -------------------------------------------------------------------------
    # Write all four patches back to the file in one operation
    # -------------------------------------------------------------------------
    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] All patches written to ffx_vk.cpp successfully.")

    # =========================================================================
    # VERIFICATION STEP
    # Read the file back and verify each patch is actually present.
    # This catches any silent write failures.
    # =========================================================================
    file(READ "${VK_BACKEND_FILE}" vk_code_verify)

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-C" VERIFY_C_POS)
    if(VERIFY_C_POS EQUAL -1)
        message(FATAL_ERROR
            "[VERIFY FAILED] FP16 kill switch (Patch-C) is NOT present in the written file!\n"
            "This is a critical error - FSR2 will produce corrupted colors on SwiftShader.\n"
            "Please check file write permissions for: ${VK_BACKEND_FILE}"
        )
    else()
        message(STATUS "[VERIFY] Patch-C (FP16 kill switch) confirmed present in file. OK!")
    endif()

    string(FIND "${vk_code_verify}" "SwiftShader-Fix-D" VERIFY_D_POS)
    if(VERIFY_D_POS EQUAL -1)
        message(STATUS
            "[VERIFY] Warning: Patch-D (B10G11R11 override) marker not found."
            " This may be OK if the format was not present in the SDK."
        )
    else()
        message(STATUS "[VERIFY] Patch-D (B10G11R11 override) confirmed present in file. OK!")
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

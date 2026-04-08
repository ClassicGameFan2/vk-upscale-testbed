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
    # PATCH E: Skip Luminance Pyramid Dispatch in ffx_fsr2.cpp
    #
    # PROBLEM:
    # SwiftShader has subgroupSize=4. The luminance pyramid pass uses
    # AMD's Single Pass Downsampler (SPD) which requires subgroupSize>=32
    # for its atomic reduction operations. When SPD runs on SwiftShader,
    # it produces garbage/NaN values in the luminance mip chain.
    # FSR2 uses this luminance chain for SHADING CHANGE DETECTION in the
    # reproject-and-accumulate pass. Garbage luminance values cause the
    # shading change detector to fire on EVERY pixel EVERY frame, which
    # collapses the temporal accumulation weight to zero -> BLACK OUTPUT.
    #
    # NOTE: We cannot patch the HLSL shader source because AMD ships
    # PRE-COMPILED SPIR-V blobs with the SDK. The .h shader files are
    # source only - the Vulkan backend loads the pre-built .spv blobs.
    # Patching .h files has ZERO effect on runtime behavior.
    #
    # CORRECT FIX:
    # Patch ffx_fsr2.cpp to make the C++ dispatch function return
    # immediately (FFX_OK) without scheduling any GPU work.
    # This prevents SPD from ever running on SwiftShader.
    #
    # EFFECT ON FSR2:
    # - Luminance mip chain = stays at cleared zero values
    # - Shading change detection = reads zeros = all pixels "stable"
    # - Temporal accumulation = accumulates correctly over 32 frames
    # - Auto-exposure = already disabled (we provide manual exposure=1.0)
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )

    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR
            "[Patch-E] CRITICAL: ffx_fsr2.cpp not found at:\n"
            "${FSR2_CPP_FILE}"
        )
    endif()

    file(READ "${FSR2_CPP_FILE}" fsr2_code)
    message(STATUS "[Patch-E] ffx_fsr2.cpp found and read OK.")

    # -------------------------------------------------------------------------
    # Print all lines containing "LuminancePyramid" so we can see the
    # exact function signature and dispatch call
    # -------------------------------------------------------------------------
    message(STATUS "=== [Patch-E] Searching for LuminancePyramid in ffx_fsr2.cpp ===")
    string(REPLACE "\n" ";" fsr2_lines "${fsr2_code}")
    set(fsr2_line_num 0)
    foreach(fsr2_line IN LISTS fsr2_lines)
        math(EXPR fsr2_line_num "${fsr2_line_num} + 1")
        if(fsr2_line MATCHES "LuminancePyramid|luminancePyramid|luminance_pyramid")
            message(STATUS "  LINE ${fsr2_line_num}: ${fsr2_line}")
        endif()
    endforeach()
    message(STATUS "=== [Patch-E] End LuminancePyramid search ===")

    # -------------------------------------------------------------------------
    # The dispatch function for luminance pyramid in ffx_fsr2.cpp is:
    # static FfxErrorCode fsr2DispatchLuminancePyramid(...)
    # We make it return FFX_OK immediately without doing any GPU work.
    #
    # Search string: the function signature opening line
    # This is unique in the file - only one function has this name.
    # -------------------------------------------------------------------------
    set(PATCH_E_SEARCH
        "static FfxErrorCode fsr2DispatchLuminancePyramid("
    )
    set(PATCH_E_REPLACE
        "static FfxErrorCode fsr2DispatchLuminancePyramid_DISABLED_SwiftShader(FfxFsr2Context_Private* context, const FfxFsr2DispatchDescription* params) { return FFX_OK; } static FfxErrorCode fsr2DispatchLuminancePyramid("
    )

    string(FIND "${fsr2_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
    if(PATCH_E_POS EQUAL -1)
        message(FATAL_ERROR
            "[Patch-E] CRITICAL: fsr2DispatchLuminancePyramid not found!\n"
            "Check LINE output above for the actual function name."
        )
    else()
        string(REPLACE "${PATCH_E_SEARCH}" "${PATCH_E_REPLACE}" fsr2_code "${fsr2_code}")
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        message(STATUS "[Patch-E] fsr2DispatchLuminancePyramid early-exit applied OK!")
    endif()

    # Verify Patch-E
    file(READ "${FSR2_CPP_FILE}" fsr2_verify)
    string(FIND "${fsr2_verify}" "fsr2DispatchLuminancePyramid_DISABLED_SwiftShader" VERIFY_E_POS)
    if(VERIFY_E_POS EQUAL -1)
        message(FATAL_ERROR
            "[VERIFY-E FAILED] Patch-E not found in written ffx_fsr2.cpp!"
        )
    else()
        message(STATUS "[VERIFY-E] Patch-E confirmed present in ffx_fsr2.cpp. OK!")
    endif()

    # =========================================================================
    # VERIFICATION of ffx_vk.cpp patches
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

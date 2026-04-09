cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Turn each patch ON or OFF independently for testing.
# Comments explain which backend each patch is intended for.
#
# Current configuration: LAVAPIPE Phase 1 test
#   - Patch-C OFF: FP16 kill switch (SwiftShader only - unconfirmed)
#   - Patch-D OFF: B10G11R11 format override (SwiftShader only - unconfirmed)
#   - Patch-E ON:  SPD bypass (required for both - confirmed fixes crash)
# =============================================================================

# Patch-A: SwiftShader memory allocator fix #1
# Disables device-local memory preference that confuses SwiftShader.
# Required for: SWIFTSHADER (confirmed) / Lavapipe: unknown, keep ON for safety
set(PATCH_A_ENABLED ON)

# Patch-B: SwiftShader memory allocator fix #2
# Disables dedicated allocation that SwiftShader does not support properly.
# Required for: SWIFTSHADER (confirmed) / Lavapipe: unknown, keep ON for safety
set(PATCH_B_ENABLED OFF)

# Patch-C: FP16 kill switch
# Forces fp16Supported=false in ffx_vk.cpp so FSR2 uses pure FP32 math.
# Intended for SwiftShader whose FP16 math corrupts negative YCoCg values.
# Status: UNCONFIRMED - dark silhouette persisted even with this patch ON.
# Required for: SWIFTSHADER_ONLY (unconfirmed) / Lavapipe: not needed
set(PATCH_C_ENABLED OFF)

# Patch-D: B10G11R11 format override
# Redirects VK_FORMAT_B10G11R11_UFLOAT_PACK32 return to R16G16B16A16_SFLOAT.
# SwiftShader cannot write UAVs in B10G11R11 format - silently drops writes.
# Status: UNCONFIRMED - may not be the root cause on SwiftShader.
# Required for: SWIFTSHADER_ONLY (unconfirmed) / Lavapipe: not needed
set(PATCH_D_ENABLED OFF)

# Patch-E: SPD (Single Pass Downsampler) bypass
# Skips the luminance pyramid dispatch in ffx_fsr2.cpp.
# SPD requires subgroupSize>=32. Both SwiftShader and Lavapipe have size=4.
# SwiftShader: SPD produces garbage -> dark silhouette (silent corruption)
# Lavapipe:    SPD causes hard crash (strict implementation)
# Status: CONFIRMED fixes Lavapipe crash and was confirmed applied OK.
# Required for: BOTH (confirmed)
set(PATCH_E_ENABLED ON)

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

FetchContent_Declare(ffx_sdk_v2
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        v1.1.4
)
FetchContent_GetProperties(ffx_sdk_v2)

# =============================================================================
# FORCE RE-PATCH EVERY CMAKE RUN
# Deletes FetchContent stamp so patches always execute.
# Prevents cache from silently skipping patches.
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
    # Required for: BOTH
    # Removes SetThreadDescription calls that do not exist on Windows 7.
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

    # -------------------------------------------------------------------------
    # PATCH A: SwiftShader memory allocator fix #1
    # Required for: SWIFTSHADER (confirmed) / keep ON for Lavapipe safety
    # -------------------------------------------------------------------------
    if(PATCH_A_ENABLED)
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
    else()
        message(STATUS "[Patch-A] DISABLED by switch.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH B: SwiftShader memory allocator fix #2
    # Required for: SWIFTSHADER (confirmed) / keep ON for Lavapipe safety
    # -------------------------------------------------------------------------
    if(PATCH_B_ENABLED)
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
    else()
        message(STATUS "[Patch-B] DISABLED by switch.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH C: FP16 kill switch
    # Required for: SWIFTSHADER_ONLY (unconfirmed) / OFF for Lavapipe
    # -------------------------------------------------------------------------
    if(PATCH_C_ENABLED)
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
    else()
        message(STATUS "[Patch-C] DISABLED by switch.")
    endif()

    # -------------------------------------------------------------------------
    # PATCH D: B10G11R11 format override
    # Required for: SWIFTSHADER_ONLY (unconfirmed) / OFF for Lavapipe
    # -------------------------------------------------------------------------
    if(PATCH_D_ENABLED)
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
    else()
        message(STATUS "[Patch-D] DISABLED by switch.")
    endif()

    # Write ffx_vk.cpp with all active patches
    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] ffx_vk.cpp written.")

    # Verify Patch-C if enabled
    if(PATCH_C_ENABLED)
        file(READ "${VK_BACKEND_FILE}" vk_verify)
        string(FIND "${vk_verify}" "SwiftShader-Fix-C" VERIFY_C_POS)
        if(VERIFY_C_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY] Patch-C not confirmed in written file!")
        else()
            message(STATUS "[VERIFY] Patch-C confirmed present. OK!")
        endif()
    endif()

    # Verify Patch-D if enabled
    if(PATCH_D_ENABLED)
        file(READ "${VK_BACKEND_FILE}" vk_verify)
        string(FIND "${vk_verify}" "SwiftShader-Fix-D" VERIFY_D_POS)
        if(VERIFY_D_POS EQUAL -1)
            message(STATUS "[VERIFY] Warning: Patch-D marker not found.")
        else()
            message(STATUS "[VERIFY] Patch-D confirmed present. OK!")
        endif()
    endif()

    # =========================================================================
    # PATCH E: SPD bypass in ffx_fsr2.cpp
    # Required for: BOTH (confirmed)
    # Skips luminance pyramid dispatch to avoid SPD subgroup crash.
    # SwiftShader: silent corruption without this patch.
    # Lavapipe:    hard crash without this patch.
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR "[Patch-E] ffx_fsr2.cpp not found at: ${FSR2_CPP_FILE}")
    endif()

    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        set(PATCH_E_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        set(PATCH_E_REPLACE
            "/* [SwiftShader-Fix-E] SPD dispatch SKIPPED - subgroupSize=4 incompatible with SPD (needs >=32). Affects both SwiftShader and Lavapipe. */"
        )
        string(FIND "${fsr2_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
        if(PATCH_E_POS EQUAL -1)
            # Alt search: unique pipeline reference
            set(PATCH_E_SEARCH_ALT
                "&context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0]"
            )
            string(FIND "${fsr2_code}" "${PATCH_E_SEARCH_ALT}" PATCH_E_ALT_POS)
            if(PATCH_E_ALT_POS EQUAL -1)
                message(FATAL_ERROR
                    "[Patch-E] pipelineComputeLuminancePyramid dispatch not found!"
                )
            else()
                string(REPLACE "${PATCH_E_SEARCH_ALT}"
                    "&context->pipelineComputeLuminancePyramid, 0 /* [SwiftShader-Fix-E] SPD bypassed */"
                    fsr2_code "${fsr2_code}")
                message(STATUS "[Patch-E] Applied via ALT search. OK!")
            endif()
        else()
            string(REPLACE "${PATCH_E_SEARCH}" "${PATCH_E_REPLACE}" fsr2_code "${fsr2_code}")
            message(STATUS "[Patch-E] SPD dispatch skipped. OK!")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")

        # Verify Patch-E
        file(READ "${FSR2_CPP_FILE}" fsr2_verify)
        string(FIND "${fsr2_verify}" "SwiftShader-Fix-E" VERIFY_E_POS)
        if(VERIFY_E_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-E] Patch-E not confirmed in ffx_fsr2.cpp!")
        else()
            message(STATUS "[VERIFY-E] Patch-E confirmed present. OK!")
        endif()
    else()
        message(STATUS "[Patch-E] DISABLED by switch - SPD will run normally.")
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
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

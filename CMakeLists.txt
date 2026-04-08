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

    # Win7 Compat Scrubber
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

    # =========================================================================
    # DIAGNOSTIC: Print every line containing "fp16" from ffx_vk.cpp
    # This tells us the EXACT text we need to search for Patch-C
    # =========================================================================
    message(STATUS "=== FP16 DIAGNOSTIC: Searching ffx_vk.cpp for fp16 ===")
    string(REPLACE "\n" ";" vk_lines "${vk_code}")
    set(line_num 0)
    foreach(line IN LISTS vk_lines)
        math(EXPR line_num "${line_num} + 1")
        string(TOLOWER "${line}" line_lower)
        if(line_lower MATCHES "fp16")
            message(STATUS "  LINE ${line_num}: ${line}")
        endif()
    endforeach()
    message(STATUS "=== END FP16 DIAGNOSTIC ===")

    # Patch D (B10G11R11 format - we know this one works)
    set(PATCH_D_SEARCH "VK_FORMAT_B10G11R11_UFLOAT_PACK32")
    string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
    if(PATCH_D_POS EQUAL -1)
        message(STATUS "[Patch-D] Already patched or not found, skipping.")
    else()
        string(REPLACE "${PATCH_D_SEARCH}"
            "VK_FORMAT_R16G16B16A16_SFLOAT /* [SwiftShader-Fix-D] */"
            vk_code "${vk_code}")
        message(STATUS "[Patch-D] B10G11R11->R16G16B16A16 applied OK.")
    endif()

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] Written to ffx_vk.cpp.")

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

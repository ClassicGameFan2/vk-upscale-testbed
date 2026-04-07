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

# --- FETCH STB_IMAGE ---
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

# --- CRITICAL FIX: BULLETPROOF FINDVULKAN INTERCEPTOR ---
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/FindVulkan.cmake "
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${vulkan_headers_SOURCE_DIR}/include\")
    endif()
    set(Vulkan_FOUND TRUE)
    set(VULKAN_FOUND TRUE)
    set(Vulkan_INCLUDE_DIRS \"${vulkan_headers_SOURCE_DIR}/include\")
    set(Vulkan_LIBRARIES Vulkan::Vulkan)
    set(Vulkan_VERSION \"1.3.250\")
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_BINARY_DIR}")

# --- ENABLE BOTH FSR 1 AND FSR 2 ---
set(FFX_FSR1 ON CACHE BOOL "" FORCE)
set(FFX_FSR2 ON CACHE BOOL "" FORCE)

set(FFX_API_BACKEND "VK_X64" CACHE STRING "" FORCE)
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        v1.1.4 
)
FetchContent_GetProperties(ffx_sdk)
if(NOT ffx_sdk_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(ffx_sdk)
    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk ${ffx_sdk_BINARY_DIR}/sdk)
endif()

# =========================================================================
# BULLETPROOF SDK SCRUBBER & TRACER (Runs every time CMake is configured!)
# =========================================================================

file(GLOB_RECURSE ffx_source_files "${ffx_sdk_SOURCE_DIR}/sdk/src/*.cpp" "${ffx_sdk_SOURCE_DIR}/sdk/src/*.h")
foreach(ffx_file IN LISTS ffx_source_files)
    file(READ ${ffx_file} file_contents)
    if(file_contents MATCHES "SetThreadDescription")
        string(REPLACE "SetThreadDescription(" "0 * sizeof(" file_contents "${file_contents}")
        file(WRITE ${ffx_file} "${file_contents}")
    endif()
endforeach()

# SWIFTSHADER MEMORY FIX, FP32 OVERRIDE & HARDCODED VK TRACES
file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" vk_backend_code)

# 1. SwiftShader Memory bypass
string(REPLACE "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))" "if (false)" vk_backend_code "${vk_backend_code}")
string(REPLACE "if (devCaps.dedicatedAllocationSupported)" "if (false)" vk_backend_code "${vk_backend_code}")

# 2. FP16 Kill Switch with HARDCODED PRINTF (This guarantees we know if the patch applied!)
string(REPLACE "deviceCapabilities->fp16Supported = (bool)shaderFloat16Int8Features.shaderFloat16;" "deviceCapabilities->fp16Supported = false; printf(\" [Trace-VK] Forced FP16 Supported = FALSE\\\\n\"); fflush(stdout);" vk_backend_code "${vk_backend_code}")

file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" "${vk_backend_code}")

# FSR 2 NUCLEAR FORMAT OVERRIDE & TRACES
file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp" fsr2_code)

# Revert to R16 (Proved successful at curing SwiftShader UAV drops without causing memory bounds errors!)
string(REPLACE "FFX_SURFACE_FORMAT_R11G11B10_FLOAT" "FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT" fsr2_code "${fsr2_code}")

file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp" "${fsr2_code}")

# We need a tiny stb implementation file to compile the headers
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp "
#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include \"stb_image_write.h\"
")

add_executable(vk-upscale-testbed src/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp)

target_include_directories(vk-upscale-testbed PRIVATE 
    ${stb_SOURCE_DIR}
    ${ffx_sdk_SOURCE_DIR}/sdk/include
    ${ffx_sdk_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr1_x64
    ffx_fsr2_x64
    ffx_backend_vk_x64
)

target_compile_definitions(ffx_backend_vk_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr1_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr2_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)

target_link_libraries(ffx_backend_vk_x64 volk::volk)

if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

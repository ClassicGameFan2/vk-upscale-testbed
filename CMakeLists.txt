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

# CRITICAL FIX: The SDK strictly checks for "VK_X64" to include the backend folder!
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

    # WINDOWS 7 COMPATIBILITY SCRUBBER
    file(GLOB_RECURSE ffx_source_files "${ffx_sdk_SOURCE_DIR}/sdk/src/*.cpp" "${ffx_sdk_SOURCE_DIR}/sdk/src/*.h")
    foreach(ffx_file IN LISTS ffx_source_files)
        file(READ ${ffx_file} file_contents)
        if(file_contents MATCHES "SetThreadDescription")
            string(REPLACE "SetThreadDescription(" "0 * sizeof(" file_contents "${file_contents}")
            file(WRITE ${ffx_file} "${file_contents}")
        endif()
    endforeach()

    # SWIFTSHADER MEMORY FIX, NULL POINTER BYPASS, & DEEP VULKAN TRACER
    file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" vk_backend_code)
    string(REPLACE "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))" "if (false)" vk_backend_code "${vk_backend_code}")
    string(REPLACE "if (devCaps.dedicatedAllocationSupported)" "if (false)" vk_backend_code "${vk_backend_code}")
    string(PREPEND vk_backend_code "#include <stdio.h>\n")
    string(REPLACE "if (backendContext->vkFunctionTable.vkCreateDescriptorPool" "printf(\" [Trace-VK] vkCreateDescriptorPool...\\n\"); fflush(stdout); if (backendContext->vkFunctionTable.vkCreateDescriptorPool" vk_backend_code "${vk_backend_code}")
    string(REPLACE "if (backendContext->vkFunctionTable.vkCreateBuffer" "printf(\" [Trace-VK] vkCreateBuffer...\\n\"); fflush(stdout); if (backendContext->vkFunctionTable.vkCreateBuffer" vk_backend_code "${vk_backend_code}")
    file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" "${vk_backend_code}")

    # DEEP FSR1 TRACER
    file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr1/ffx_fsr1.cpp" fsr1_code)
    string(PREPEND fsr1_code "#include <stdio.h>\n")
    string(REPLACE "FfxVersionNumber version = context->contextDescription.backendInterface.fpGetSDKVersion" "printf(\" [Trace-FSR1] fpGetSDKVersion...\\n\"); fflush(stdout); FfxVersionNumber version = context->contextDescription.backendInterface.fpGetSDKVersion" fsr1_code "${fsr1_code}")
    string(REPLACE "FfxErrorCode errorCode =\n        context->contextDescription.backendInterface.fpCreateBackendContext" "printf(\" [Trace-FSR1] fpCreateBackendContext...\\n\"); fflush(stdout); FfxErrorCode errorCode =\n        context->contextDescription.backendInterface.fpCreateBackendContext" fsr1_code "${fsr1_code}")
    string(REPLACE "errorCode = context->contextDescription.backendInterface.fpGetDeviceCapabilities" "printf(\" [Trace-FSR1] fpGetDeviceCapabilities...\\n\"); fflush(stdout); errorCode = context->contextDescription.backendInterface.fpGetDeviceCapabilities" fsr1_code "${fsr1_code}")
    string(REPLACE "FFX_VALIDATE(context->contextDescription.backendInterface.fpCreateResource" "printf(\" [Trace-FSR1] fpCreateResource (RCAS Output)...\\n\"); fflush(stdout); FFX_VALIDATE(context->contextDescription.backendInterface.fpCreateResource" fsr1_code "${fsr1_code}")
    string(REPLACE "errorCode = createPipelineStates(context);" "printf(\" [Trace-FSR1] createPipelineStates...\\n\"); fflush(stdout); errorCode = createPipelineStates(context);" fsr1_code "${fsr1_code}")
    file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr1/ffx_fsr1.cpp" "${fsr1_code}")

    # --- FSR 2 PURE 32-BIT FLOAT OVERRIDE ---
    # This totally bypasses SwiftShader's 16-bit half-float corruption bugs!
    file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp" fsr2_code)
    string(REPLACE "FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT" "FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT" fsr2_code "${fsr2_code}")
    string(REPLACE "FFX_SURFACE_FORMAT_R16G16_FLOAT" "FFX_SURFACE_FORMAT_R32G32_FLOAT" fsr2_code "${fsr2_code}")
    string(REPLACE "FFX_SURFACE_FORMAT_R16_FLOAT" "FFX_SURFACE_FORMAT_R32_FLOAT" fsr2_code "${fsr2_code}")
    string(REPLACE "FFX_SURFACE_FORMAT_R11G11B10_FLOAT" "FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT" fsr2_code "${fsr2_code}")
    file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp" "${fsr2_code}")

    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk ${ffx_sdk_BINARY_DIR}/sdk)
endif()

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

# Link both FSR1 and FSR2
target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr1_x64
    ffx_fsr2_x64
    ffx_backend_vk_x64
)

# PREVENT SILENT CRASHES!
target_compile_definitions(ffx_backend_vk_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr1_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr2_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)

target_link_libraries(ffx_backend_vk_x64 volk::volk)

if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

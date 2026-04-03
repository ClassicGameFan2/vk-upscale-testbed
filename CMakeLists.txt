cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG main
)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG master
)
FetchContent_MakeAvailable(volk)

# --- CRITICAL FIX: TRICK AMD INTO FINDING OUR VULKAN HEADERS ---
# AMD's script runs find_package(Vulkan). If it fails, it silently skips the Vulkan backend!
# We create a dummy FindVulkan.cmake that points directly to our downloaded headers.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/FindVulkan.cmake "
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${vulkan_headers_SOURCE_DIR}/include\")
    endif()
    set(Vulkan_FOUND TRUE)
    set(VULKAN_FOUND TRUE)
")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_BINARY_DIR}")

# --- THE ULTIMATE BYPASS ---
set(FFX_FSR2 ON CACHE BOOL "" FORCE)
set(FFX_BACKEND_VK ON CACHE BOOL "" FORCE)
set(FFX_API_VULKAN ON CACHE BOOL "" FORCE)
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG v1.1.4 
)
FetchContent_GetProperties(ffx_sdk)
if(NOT ffx_sdk_POPULATED)
    FetchContent_Populate(ffx_sdk)
    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk ${ffx_sdk_BINARY_DIR}/sdk)
endif()

add_executable(vk-upscale-testbed src/main.cpp)

target_include_directories(vk-upscale-testbed PRIVATE 
    ${ffx_sdk_SOURCE_DIR}/sdk/include
    ${ffx_sdk_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr2_x64 
    ffx_backend_vk_x64
)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

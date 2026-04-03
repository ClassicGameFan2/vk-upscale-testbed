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
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/FindVulkan.cmake "
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${vulkan_headers_SOURCE_DIR}/include\")
    endif()
    set(Vulkan_FOUND TRUE)
    set(VULKAN_FOUND TRUE)
")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_BINARY_DIR}")

FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG v1.1.4 
)
FetchContent_GetProperties(ffx_sdk)
if(NOT ffx_sdk_POPULATED)
    FetchContent_Populate(ffx_sdk)
    
    # --- THE SURGICAL STRIKE ---
    # We bypass all of AMD's wrappers and compile the exact components we need!
    set(FFX_API_VK ON CACHE BOOL "" FORCE)
    
    # 1. Compile the Vulkan Backend
    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk ${ffx_sdk_BINARY_DIR}/sdk/src/backends/vk)
    
    # 2. Compile the FSR2 Core
    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk/src/components/fsr2 ${ffx_sdk_BINARY_DIR}/sdk/src/components/fsr2)
endif()

add_executable(vk-upscale-testbed src/main.cpp)

target_include_directories(vk-upscale-testbed PRIVATE 
    ${ffx_sdk_SOURCE_DIR}/sdk/include
    ${ffx_sdk_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

# Link the exact _x64 targets that AMD generates from those specific folders
target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr2_x64 
    ffx_backend_vk_x64
)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

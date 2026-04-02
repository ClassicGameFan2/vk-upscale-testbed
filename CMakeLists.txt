cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

# 1. Auto-Download Official Vulkan Headers
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(vulkan_headers)

# 2. Auto-Download Volk (Vulkan Meta-Loader)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(volk)

# 3. Auto-Download AMD FidelityFX Unified SDK!
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
set(FFX_API_VULKAN ON CACHE BOOL "" FORCE)
set(FFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(ffx_sdk)

add_executable(vk-upscale-testbed src/main.cpp)

# Link Vulkan and Volk (VMA is gone!)
target_link_libraries(vk-upscale-testbed PRIVATE volk::volk_headers volk::volk)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

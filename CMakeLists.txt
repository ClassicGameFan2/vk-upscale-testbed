cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)

# --- CRITICAL FIX: THE NUCLEAR OPTION ---
# AMD's script randomly checks multiple different variables and fails if they are empty.
# We forcefully hardcode every possible variable it could ask for!
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
set(BUILD_TYPE "Release" CACHE STRING "" FORCE)
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)

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

set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
set(FFX_API_VULKAN ON CACHE BOOL "" FORCE)
set(FFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        v1.1.4 
)
FetchContent_MakeAvailable(ffx_sdk)

add_executable(vk-upscale-testbed src/main.cpp)

target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr2 
    ffx_backend_vk
)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

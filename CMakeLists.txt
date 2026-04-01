cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

# Auto-Download Official Vulkan Headers
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(vulkan_headers)

# Auto-Download Volk (Vulkan Meta-Loader)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(volk)

# Build our executable
add_executable(vk-upscale-testbed src/main.cpp)

# Link Vulkan and Volk
target_link_libraries(vk-upscale-testbed PRIVATE volk::volk_headers volk::volk)

# Ensure it runs on Windows 7 without missing visual c++ redistributables
if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

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

# --- THE MASTER HACK: INTERCEPT FIND_PACKAGE ---
# AMD's script requires the massive Vulkan SDK to be installed.
# We intercept their check, point it to our lightweight headers, and force success!
macro(find_package name)
    if("${name}" STREQUAL "Vulkan")
        if(NOT TARGET Vulkan::Vulkan)
            add_library(Vulkan::Vulkan INTERFACE IMPORTED)
            set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${vulkan_headers_SOURCE_DIR}/include")
        endif()
        set(Vulkan_FOUND TRUE)
        set(VULKAN_FOUND TRUE)
    else()
        _find_package(${ARGV}) # Route all other checks back to standard CMake
    endif()
endmacro()

# --- CONFIGURE AMD SDK ---
set(FFX_FSR2 ON CACHE BOOL "" FORCE)
set(FFX_API_VK ON CACHE BOOL "" FORCE)
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

# Link the pure targets so CMake auto-resolves the _x64 names
target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr2 
    ffx_backend_vk
)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

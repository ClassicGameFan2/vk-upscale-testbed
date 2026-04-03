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
    set(Vulkan_INCLUDE_DIRS \"${vulkan_headers_SOURCE_DIR}/include\")
    set(Vulkan_LIBRARIES Vulkan::Vulkan)
    set(Vulkan_VERSION \"1.3.250\")
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_BINARY_DIR}")

# --- THE ULTIMATE BYPASS ---
set(FFX_FSR2 ON CACHE BOOL "" FORCE)
set(FFX_API_VK ON CACHE BOOL "" FORCE)
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
set(FFX_API_BACKEND "VK_X64" CACHE STRING "" FORCE)

FetchContent_Declare(ffx_sdk
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG v1.1.4 
)
FetchContent_GetProperties(ffx_sdk)
if(NOT ffx_sdk_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(ffx_sdk)
    
    # --- CRITICAL FIX: WINDOWS 7 COMPATIBILITY SCRUBBER ---
    # Search all AMD source files and remove the Windows 10 SetThreadDescription API.
    # We replace it with '0 * (' which turns the function call into a harmless math equation!
    file(GLOB_RECURSE ffx_source_files "${ffx_sdk_SOURCE_DIR}/sdk/src/*.cpp" "${ffx_sdk_SOURCE_DIR}/sdk/src/*.h")
    foreach(ffx_file IN LISTS ffx_source_files)
        file(READ ${ffx_file} file_contents)
        if(file_contents MATCHES "SetThreadDescription")
            string(REPLACE "SetThreadDescription(" "0 * (" file_contents "${file_contents}")
            file(WRITE ${ffx_file} "${file_contents}")
        endif()
    endforeach()

    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk ${ffx_sdk_BINARY_DIR}/sdk)
endif()

add_executable(vk-upscale-testbed src/main.cpp)

target_include_directories(vk-upscale-testbed PRIVATE 
    ${ffx_sdk_SOURCE_DIR}/sdk/include
    ${ffx_sdk_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

# Link the exact _x64 targets that AMD generates
target_link_libraries(vk-upscale-testbed PRIVATE 
    volk::volk_headers 
    volk::volk 
    ffx_fsr2_x64 
    ffx_backend_vk_x64
)

if(MSVC)
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

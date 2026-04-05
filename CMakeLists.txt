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

# --- FETCH STB_IMAGE ---
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
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
    GIT_TAG v1.1.4 
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

    # SWIFTSHADER MEMORY FIX: Force AMD to accept Host Visible VRAM!
    file(READ "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" vk_backend_code)
    string(REPLACE "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))" "if (false)" vk_backend_code "${vk_backend_code}")
    file(WRITE "${ffx_sdk_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp" "${vk_backend_code}")

    add_subdirectory(${ffx_sdk_SOURCE_DIR}/sdk ${ffx_sdk_BINARY_DIR}/sdk)
endif()

# We need a tiny stb implementation file to compile the headers
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp "
    #define STB_IMAGE_IMPLEMENTATION
    #define STB_IMAGE_WRITE_IMPLEMENTATION
    #include \"stb_image.h\"
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

target_link_libraries(ffx_backend_vk_x64 volk::volk)
if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

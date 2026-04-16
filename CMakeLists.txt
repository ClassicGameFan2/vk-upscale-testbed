cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
set(PATCH_A_ENABLED OFF)
set(PATCH_B_ENABLED OFF)
set(PATCH_C_ENABLED OFF)
set(PATCH_D_ENABLED OFF)
set(PATCH_E_ENABLED OFF)
set(PATCH_F_ENABLED OFF)

# =============================================================================
# PASS ISOLATION SWITCHES
# =============================================================================
set(PASS_SPD_ENABLED          ON)
set(PASS_RECONSTRUCT_ENABLED  ON)
set(PASS_DEPTHCLIP_ENABLED    ON)
set(PASS_LOCK_ENABLED         ON)
set(PASS_ACCUMULATE_ENABLED   ON)
set(PASS_SPD_FORCE_SINGLE_WORKGROUP OFF)

set(FFX_USE_PRECOMPILED_SHADERS ON)

# =============================================================================
# DEPENDENCIES
# =============================================================================
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_Tag        master
)
FetchContent_MakeAvailable(volk)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/FindVulkan.cmake "
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES
            \"${vulkan_headers_SOURCE_DIR}/include\")
    endif()
    set(Vulkan_FOUND TRUE)
    set(VULKAN_FOUND TRUE)
    set(Vulkan_INCLUDE_DIRS \"${vulkan_headers_SOURCE_DIR}/include\")
    set(Vulkan_LIBRARIES Vulkan::Vulkan)
    set(Vulkan_VERSION \"1.3.250\")
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_BINARY_DIR}")

set(FFX_FSR1 ON CACHE BOOL "" FORCE)
set(FFX_FSR2 ON CACHE BOOL "" FORCE)
set(FFX_FSR3 ON CACHE BOOL "" FORCE)
set(FFX_API_BACKEND "VK_X64" CACHE STRING "" FORCE)
set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
set(FFX_USE_PRECOMPILED_SHADERS ${FFX_USE_PRECOMPILED_SHADERS}
    CACHE BOOL "" FORCE)

FetchContent_Declare(ffx_sdk_v2
    GIT_REPOSITORY https://github.com/ClassicGameFan2/FidelityFX-SDK.git
    GIT_TAG        MinChanges
)
FetchContent_GetProperties(ffx_sdk_v2)

# Force re-patch every CMake run
set(FFX_STAMP_FILE
    "${CMAKE_BINARY_DIR}/_deps/ffx_sdk_v2-subbuild/ffx_sdk_v2-populate-prefix/src/ffx_sdk_v2-populate-stamp/ffx_sdk_v2-populate-done"
)
if(EXISTS "${FFX_STAMP_FILE}")
    message(STATUS "[Patch] Deleting FetchContent stamp to force re-patch...")
    file(REMOVE "${FFX_STAMP_FILE}")
endif()

if(NOT ffx_sdk_v2_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(ffx_sdk_v2)

    # =========================================================================
    # WIN7 COMPAT SCRUBBER - Required for: BOTH
    # =========================================================================
    file(GLOB_RECURSE ffx_source_files
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/*.h"
    )
    foreach(ffx_file IN LISTS ffx_source_files)
        file(READ ${ffx_file} file_contents)
        if(file_contents MATCHES "SetThreadDescription")
            string(REPLACE
                "SetThreadDescription("
                "0 * sizeof("
                file_contents "${file_contents}"
            )
            file(WRITE ${ffx_file} "${file_contents}")
            message(STATUS "[Patch] Win7 scrub: ${ffx_file}")
        endif()
    endforeach()

    # =========================================================================
    # DIAGNOSTIC INJECTOR A: ffx_vk.cpp - line number on every error return
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
    file(READ "${VK_BACKEND_FILE}" vk_code)

    string(PREPEND vk_code "#include <stdio.h>\n")

    string(REPLACE
        "return FFX_ERROR_BACKEND_API_ERROR;"
        "{ printf(\"[VK-DIAG] ffx_vk.cpp error at line %d\\n\", __LINE__); fflush(stdout); return FFX_ERROR_BACKEND_API_ERROR; }"
        vk_code "${vk_code}"
    )

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Diag-A] ffx_vk.cpp error line injector applied.")

    # =========================================================================
    # PATCH ffx_vk.cpp (Patches A, B, C, D)
    # =========================================================================
    file(READ "${VK_BACKEND_FILE}" vk_code)

    if(PATCH_A_ENABLED)
        set(PATCH_A_SEARCH
            "if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"
        )
        string(FIND "${vk_code}" "${PATCH_A_SEARCH}" PATCH_A_POS)
        if(PATCH_A_POS EQUAL -1)
            message(STATUS "[Patch-A] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_A_SEARCH}"
                "if (false) /* [Fix-A] Device-local memory preference disabled */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-A] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-A] DISABLED.")
    endif()

    if(PATCH_B_ENABLED)
        set(PATCH_B_SEARCH "if (devCaps.dedicatedAllocationSupported)")
        string(FIND "${vk_code}" "${PATCH_B_SEARCH}" PATCH_B_POS)
        if(PATCH_B_POS EQUAL -1)
            message(STATUS "[Patch-B] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_B_SEARCH}"
                "if (false) /* [Fix-B] Dedicated allocation disabled */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-B] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-B] DISABLED.")
    endif()

    if(PATCH_C_ENABLED)
        set(PATCH_C_SEARCH
            "deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16"
        )
        string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
        if(PATCH_C_POS EQUAL -1)
            message(FATAL_ERROR "[Patch-C] Search string not found!")
        else()
            string(REPLACE "${PATCH_C_SEARCH}"
                "deviceCapabilities->fp16Supported = false /* [Fix-C] FP16 kill switch */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-C] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-C] DISABLED.")
    endif()

    if(PATCH_D_ENABLED)
        set(PATCH_D_SEARCH "return VK_FORMAT_B10G11R11_UFLOAT_PACK32")
        string(FIND "${vk_code}" "${PATCH_D_SEARCH}" PATCH_D_POS)
        if(PATCH_D_POS EQUAL -1)
            message(STATUS "[Patch-D] Already patched, skipping.")
        else()
            string(REPLACE "${PATCH_D_SEARCH}"
                "return VK_FORMAT_R16G16B16A16_SFLOAT /* [Fix-D] B10G11R11 override */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-D] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-D] DISABLED.")
    endif()

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] ffx_vk.cpp written.")

    # =========================================================================
    # PATCH E / F - OFF
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        message(STATUS "[Patch-E] ENABLED.")
    else()
        message(STATUS "[Patch-E] DISABLED.")
    endif()

    file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")

    if(PATCH_F_ENABLED)
        message(STATUS "[Patch-F] ENABLED.")
    else()
        message(STATUS "[Patch-F] DISABLED.")
    endif()

    # =========================================================================
    # PASS ISOLATION INJECTOR: FSR2
    # =========================================================================
    file(READ "${FSR2_CPP_FILE}" fsr2_isolation_code)

    if(NOT PASS_SPD_ENABLED)
        message(STATUS "[Isolate] SPD luminance pyramid DISABLED.")
        string(REPLACE
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);"
            "/* [ISOLATE] SPD disabled */ (void)dispatchThreadGroupCountXY;"
            fsr2_isolation_code "${fsr2_isolation_code}"
        )
    endif()

    if(NOT PASS_RECONSTRUCT_ENABLED)
        message(STATUS "[Isolate] ReconstructPreviousDepth DISABLED.")
        string(REPLACE
            "scheduleDispatch(context, params, &context->pipelineReconstructPreviousDepth, dispatchSrcX, dispatchSrcY);"
            "/* [ISOLATE] ReconstructPreviousDepth disabled */"
            fsr2_isolation_code "${fsr2_isolation_code}"
        )
    endif()

    if(NOT PASS_DEPTHCLIP_ENABLED)
        message(STATUS "[Isolate] DepthClip DISABLED.")
        string(REPLACE
            "scheduleDispatch(context, params, &context->pipelineDepthClip, dispatchSrcX, dispatchSrcY);"
            "/* [ISOLATE] DepthClip disabled */"
            fsr2_isolation_code "${fsr2_isolation_code}"
        )
    endif()

    if(NOT PASS_LOCK_ENABLED)
        message(STATUS "[Isolate] Lock DISABLED.")
        string(REPLACE
            "scheduleDispatch(context, params, &context->pipelineLock, dispatchSrcX, dispatchSrcY);"
            "/* [ISOLATE] Lock disabled */"
            fsr2_isolation_code "${fsr2_isolation_code}"
        )
    endif()

    if(NOT PASS_ACCUMULATE_ENABLED)
        message(STATUS "[Isolate] Accumulate DISABLED.")
        string(REPLACE
            "scheduleDispatch(context, params, sharpenEnabled ? &context->pipelineAccumulateSharpen : &context->pipelineAccumulate, dispatchDstX, dispatchDstY);"
            "/* [ISOLATE] Accumulate disabled */"
            fsr2_isolation_code "${fsr2_isolation_code}"
        )
    endif()

    file(WRITE "${FSR2_CPP_FILE}" "${fsr2_isolation_code}")
    message(STATUS "[Isolate] Pass isolation applied.")

    # =========================================================================
    # DIAGNOSTIC INJECTOR B: ffx_fsr3upscaler.cpp - trace every scheduled pass
    # =========================================================================
    set(FSR3UP_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    )
    file(READ "${FSR3UP_CPP_FILE}" fsr3up_code)

    # Inject stdio include at top
    string(PREPEND fsr3up_code "#include <stdio.h>\n")

    # The FSR3 upscaler schedules GPU jobs via scheduleDispatch (or similar).
    # We inject a printf wherever ffxFsr3UpscalerContextDispatch is defined
    # to announce entry, and trace each pipeline dispatch.
    # First: announce dispatch entry
    string(REPLACE
        "FfxErrorCode ffxFsr3UpscalerContextDispatch("
        "FfxErrorCode ffxFsr3UpscalerContextDispatch_ORIG(\0"  # dummy to find it
        fsr3up_code_test "${fsr3up_code}"
    )
    # Instead, inject at the top of the dispatch function body.
    # We look for a unique string near the start of the dispatch function.
    string(REPLACE
        "FFX_RETURN_ON_ERROR(pContext, FFX_ERROR_INVALID_POINTER);"
        "FFX_RETURN_ON_ERROR(pContext, FFX_ERROR_INVALID_POINTER); printf(\"[FSR3-DIAG] ffxFsr3UpscalerContextDispatch ENTERED frame reset=%d sharpen=%d\\n\", (int)pDispatchDescription->reset, (int)pDispatchDescription->enableSharpening); fflush(stdout);"
        fsr3up_code "${fsr3up_code}"
    )

    # Trace when the RCAS pipeline is scheduled - look for the RCAS dispatch call.
    # In the FSR3 source, RCAS is dispatched with pipelineRCAS or similar name.
    string(REPLACE
        "pipelineRCAS"
        "pipelineRCAS); printf(\"[FSR3-DIAG] RCAS pass scheduled\\n\"); fflush(stdout); (void)(0"
        fsr3up_code "${fsr3up_code}"
    )

    file(WRITE "${FSR3UP_CPP_FILE}" "${fsr3up_code}")
    message(STATUS "[Diag-B] ffx_fsr3upscaler.cpp diagnostic injector applied.")

    # =========================================================================
    # WIN7 COMPAT SCRUBBER for FSR3 source files
    # =========================================================================
    file(GLOB_RECURSE ffx_fsr3_source_files
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr3upscaler/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr3upscaler/*.h"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr3/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr3/*.h"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/opticalflow/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/opticalflow/*.h"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/frameinterpolation/*.cpp"
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/frameinterpolation/*.h"
    )
    foreach(ffx_file IN LISTS ffx_fsr3_source_files)
        file(READ ${ffx_file} file_contents)
        if(file_contents MATCHES "SetThreadDescription")
            string(REPLACE
                "SetThreadDescription("
                "0 * sizeof("
                file_contents "${file_contents}"
            )
            file(WRITE ${ffx_file} "${file_contents}")
            message(STATUS "[Patch] Win7 scrub FSR3: ${ffx_file}")
        endif()
    endforeach()

    add_subdirectory(${ffx_sdk_v2_SOURCE_DIR}/sdk ${ffx_sdk_v2_BINARY_DIR}/sdk)
endif()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp "
#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include \"stb_image_write.h\"
")

add_executable(vk-upscale-testbed
    src/main.cpp
    src/common.cpp
    src/fsr1_pass.cpp
    src/fsr2_pass.cpp
    src/fsr3_pass.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp
)

target_include_directories(vk-upscale-testbed PRIVATE
    ${stb_SOURCE_DIR}
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vk-upscale-testbed PRIVATE
    volk::volk_headers
    volk::volk
    ffx_fsr1_x64
    ffx_fsr2_x64
    ffx_fsr3upscaler_x64
    ffx_backend_vk_x64
)

target_compile_definitions(ffx_backend_vk_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr1_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr2_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr3upscaler_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)

target_link_libraries(ffx_backend_vk_x64 volk::volk)

if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )
endif()

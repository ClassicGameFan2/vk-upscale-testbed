cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# All SDK-level fixes now live in the forked repo branch "changes".
# CMake patches retained only for diagnostics and config-level toggles.
# =============================================================================

set(PATCH_A_ENABLED OFF) # Moved to fork: two-pass memory selection in ffx_vk.cpp
set(PATCH_B_ENABLED OFF) # Not needed (confirmed)
set(PATCH_C_ENABLED OFF) # Not needed (confirmed)
set(PATCH_D_ENABLED OFF) # Not needed (confirmed)
set(PATCH_E_ENABLED OFF) # Not needed (replaced by fork Fix-F)
set(PATCH_F_ENABLED OFF) # Moved to fork: SPD counter reset in ffx_fsr2.cpp

# =============================================================================
# PASS ISOLATION SWITCHES - for crash diagnosis only
# Disable passes one at a time to find which shader crashes on Lavapipe.
# ALL should be ON for normal operation.
# =============================================================================
set(PASS_SPD_ENABLED          ON)  # Luminance pyramid (SPD)
set(PASS_RECONSTRUCT_ENABLED  ON)  # ReconstructPreviousDepth
set(PASS_DEPTHCLIP_ENABLED    ON)  # DepthClip
set(PASS_LOCK_ENABLED         ON)  # Lock
set(PASS_ACCUMULATE_ENABLED   ON)  # Accumulate
set(PASS_SPD_FORCE_SINGLE_WORKGROUP OFF)  # Force SPD to 1 workgroup (skips atomics)

set(FFX_USE_PRECOMPILED_SHADERS OFF)

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
    # Same technique that found the VRAM crash at line 930.
    # Injects printf(__LINE__) before every FFX_ERROR_BACKEND_API_ERROR return.
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
    file(READ "${VK_BACKEND_FILE}" vk_code)

    # Prepend stdio include for printf
    string(PREPEND vk_code "#include <stdio.h>\n")

    # Inject line number printer on every backend error return
    string(REPLACE
        "return FFX_ERROR_BACKEND_API_ERROR;"
        "{ printf(\"[VK-DIAG] ffx_vk.cpp error at line %d\\n\", __LINE__); fflush(stdout); return FFX_ERROR_BACKEND_API_ERROR; }"
        vk_code "${vk_code}"
    )

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Diag-A] ffx_vk.cpp error line injector applied.")

    # =========================================================================
    # DIAGNOSTIC INJECTOR B: ffx_fsr2.cpp - checkpoints around SPD dispatch
    # Prints exactly which GPU job is being scheduled before the crash.
    # Also prints the counter clear confirmation.
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    # Inject checkpoint before the SPD counter clear job schedule call.
    # Search for the exact label we put in our Fix-F clear job.
    string(REPLACE
        "wcscpy_s(clearSpdCounter.jobLabel, L\"Clear SPD Atomic Counter\");"
        "printf(\"[FSR2-DIAG] CP1: Scheduling SPD counter clear job...\\n\"); fflush(stdout); wcscpy_s(clearSpdCounter.jobLabel, L\"Clear SPD Atomic Counter\");"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint after the SPD counter clear is scheduled
    string(REPLACE
        "context->contextDescription.backendInterface.fpScheduleGpuJob(\n            &context->contextDescription.backendInterface, &clearSpdCounter);"
        "context->contextDescription.backendInterface.fpScheduleGpuJob(\n            &context->contextDescription.backendInterface, &clearSpdCounter); printf(\"[FSR2-DIAG] CP2: SPD counter clear job scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before luminance pyramid dispatch
    string(REPLACE
        "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);"
        "printf(\"[FSR2-DIAG] CP3: Scheduling luminance pyramid dispatch [%u x %u]...\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); printf(\"[FSR2-DIAG] CP4: Luminance pyramid scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before reconstruct previous depth dispatch
    string(REPLACE
        "scheduleDispatch(context, params, &context->pipelineReconstructPreviousDepth, dispatchSrcX, dispatchSrcY);"
        "printf(\"[FSR2-DIAG] CP5: Scheduling reconstructPreviousDepth [%d x %d]...\\n\", dispatchSrcX, dispatchSrcY); fflush(stdout); scheduleDispatch(context, params, &context->pipelineReconstructPreviousDepth, dispatchSrcX, dispatchSrcY); printf(\"[FSR2-DIAG] CP6: reconstructPreviousDepth scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before depth clip dispatch
    string(REPLACE
        "scheduleDispatch(context, params, &context->pipelineDepthClip, dispatchSrcX, dispatchSrcY);"
        "printf(\"[FSR2-DIAG] CP7: Scheduling depthClip [%d x %d]...\\n\", dispatchSrcX, dispatchSrcY); fflush(stdout); scheduleDispatch(context, params, &context->pipelineDepthClip, dispatchSrcX, dispatchSrcY); printf(\"[FSR2-DIAG] CP8: depthClip scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before lock dispatch
    string(REPLACE
        "scheduleDispatch(context, params, &context->pipelineLock, dispatchSrcX, dispatchSrcY);"
        "printf(\"[FSR2-DIAG] CP9: Scheduling lock [%d x %d]...\\n\", dispatchSrcX, dispatchSrcY); fflush(stdout); scheduleDispatch(context, params, &context->pipelineLock, dispatchSrcX, dispatchSrcY); printf(\"[FSR2-DIAG] CP10: lock scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before accumulate dispatch
    string(REPLACE
        "scheduleDispatch(context, params, sharpenEnabled ? &context->pipelineAccumulateSharpen : &context->pipelineAccumulate, dispatchDstX, dispatchDstY);"
        "printf(\"[FSR2-DIAG] CP11: Scheduling accumulate [%d x %d]...\\n\", dispatchDstX, dispatchDstY); fflush(stdout); scheduleDispatch(context, params, sharpenEnabled ? &context->pipelineAccumulateSharpen : &context->pipelineAccumulate, dispatchDstX, dispatchDstY); printf(\"[FSR2-DIAG] CP12: accumulate scheduled OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    # Inject checkpoint before fpExecuteGpuJobs
    string(REPLACE
        "context->contextDescription.backendInterface.fpExecuteGpuJobs(&context->contextDescription.backendInterface, commandList, context->effectContextId);"
        "printf(\"[FSR2-DIAG] CP13: Calling fpExecuteGpuJobs...\\n\"); fflush(stdout); context->contextDescription.backendInterface.fpExecuteGpuJobs(&context->contextDescription.backendInterface, commandList, context->effectContextId); printf(\"[FSR2-DIAG] CP14: fpExecuteGpuJobs returned OK.\\n\"); fflush(stdout);"
        fsr2_code "${fsr2_code}"
    )

    file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
    message(STATUS "[Diag-B] ffx_fsr2.cpp checkpoint injector applied.")

    # =========================================================================
    # DIAGNOSTIC INJECTOR C: ffx_vk.cpp - trace every vkQueueSubmit call
    # This will show us if the VK backend itself is submitting multiple times.
    # =========================================================================
    file(READ "${VK_BACKEND_FILE}" vk_code2)

    string(REPLACE
        "vkQueueSubmit("
        "printf(\"[VK-DIAG] vkQueueSubmit called at line %d\\n\", __LINE__); fflush(stdout); vkQueueSubmit("
        vk_code2 "${vk_code2}"
    )

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code2}")
    message(STATUS "[Diag-C] ffx_vk.cpp vkQueueSubmit tracer applied.")

    # =========================================================================
    # PATCH ffx_vk.cpp (Patches A, B, C, D) - all OFF
    # =========================================================================
    set(VK_BACKEND_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp"
    )
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
    # PATCH E: C++ SPD dispatch bypass - OFF
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        # ... patch E logic unchanged, omitted for brevity ...
        message(STATUS "[Patch-E] ENABLED.")
    else()
        message(STATUS "[Patch-E] DISABLED.")
    endif()

    file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")

    # =========================================================================
    # PATCH F - OFF (fix lives in fork)
    # =========================================================================
    if(PATCH_F_ENABLED)
        message(STATUS "[Patch-F] ENABLED.")
    else()
        message(STATUS "[Patch-F] DISABLED.")
    endif()

    # =========================================================================
    # DXC SETUP
    # =========================================================================
    if(NOT FFX_USE_PRECOMPILED_SHADERS)
        message(STATUS "[Shader] Precompiled shaders DISABLED.")
        message(STATUS "[Shader] Searching for DXC compiler...")

        find_program(DXC_PATH
            NAMES dxc.exe dxc
            HINTS
                "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC"
                "C:/Program Files (x86)/Windows Kits/10/bin"
            PATH_SUFFIXES "x64" "bin/Hostx64/x64"
            DOC "Path to DXC shader compiler"
        )

        if(NOT DXC_PATH)
            message(STATUS "[Shader] find_program did not locate DXC.")
            message(STATUS "[Shader] Trying glob search...")
            file(GLOB_RECURSE DXC_CANDIDATES
                "C:/Program Files/Microsoft Visual Studio/2022/*/dxc.exe"
                "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
            )
            if(DXC_CANDIDATES)
                list(GET DXC_CANDIDATES 0 DXC_PATH)
                message(STATUS "[Shader] DXC found via glob: ${DXC_PATH}")
            else()
                message(FATAL_ERROR "[Shader] DXC compiler not found!")
            endif()
        else()
            message(STATUS "[Shader] DXC found: ${DXC_PATH}")
        endif()

        set(DXC_PATH "${DXC_PATH}" CACHE STRING "" FORCE)
        message(STATUS "[Shader] DXC path set: ${DXC_PATH}")
    else()
        message(STATUS "[Shader] Using precompiled SPIR-V blobs.")
    endif()

    # =========================================================================
    # DIAGNOSTIC: Force SPD to single workgroup to test if atomic is the crash
    # When numworkGroups=1, SPD's SpdExitWorkgroup() returns immediately
    # without ever calling SpdIncreaseAtomicCounter().
    # If this does not crash, the atomic counter IS the crash cause.
    # If this still crashes, something else in SPD crashes Lavapipe.
    # =========================================================================
    if(PASS_SPD_FORCE_SINGLE_WORKGROUP)
        file(READ "${FSR2_CPP_FILE}" fsr2_spd_test_code)
        string(REPLACE
            "luminancePyramidConstants.numworkGroups = numWorkGroupsAndMips[0];"
            "luminancePyramidConstants.numworkGroups = 1; /* [SPD-TEST] forced single workgroup */"
            fsr2_spd_test_code "${fsr2_spd_test_code}"
        )
        string(REPLACE
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);"
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, 1, 1); /* [SPD-TEST] forced single workgroup dispatch */"
            fsr2_spd_test_code "${fsr2_spd_test_code}"
        )
        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_spd_test_code}")
        message(STATUS "[SPD-TEST] Single workgroup force applied.")
    endif()

    # =========================================================================
    # PASS ISOLATION INJECTOR: comment out individual passes in fsr2Dispatch
    # to find which GPU shader causes the vkQueueWaitIdle crash.
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

    add_subdirectory(${ffx_sdk_v2_SOURCE_DIR}/sdk ${ffx_sdk_v2_BINARY_DIR}/sdk)
endif()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp "
#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include \"stb_image_write.h\"
")

add_executable(vk-upscale-testbed
    src/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/stb_impl.cpp
)

target_include_directories(vk-upscale-testbed PRIVATE
    ${stb_SOURCE_DIR}
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include
    ${ffx_sdk_v2_SOURCE_DIR}/sdk/include/FidelityFX/host/backends
)

target_link_libraries(vk-upscale-testbed PRIVATE
    volk::volk_headers
    volk::volk
    ffx_fsr1_x64
    ffx_fsr2_x64
    ffx_backend_vk_x64
)

target_compile_definitions(ffx_backend_vk_x64 PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr1_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)
target_compile_definitions(ffx_fsr2_x64       PRIVATE DISABLE_FFX_DEBUG_BREAK=1)

target_link_libraries(ffx_backend_vk_x64 volk::volk)

if(MSVC)
    target_compile_options(ffx_backend_vk_x64 PRIVATE "/FIvolk.h")
    set_property(TARGET vk-upscale-testbed PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )
endif()

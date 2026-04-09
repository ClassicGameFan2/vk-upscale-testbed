cmake_minimum_required(VERSION 3.14)
project(VkUpscaleTestbed)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
include(FetchContent)

# =============================================================================
# PATCH CONTROL SWITCHES
# =============================================================================
# Current configuration: LAVAPIPE Phase 2 - SPD crash debug
#   Patch-A ON:  Required for both backends (context creation)
#   Patch-B OFF: Not required, Lavapipe faster without it
#   Patch-C OFF: Confirmed not needed (not root cause of any bug)
#   Patch-D OFF: Confirmed not needed (not root cause of any bug)
#   Patch-E OFF: SPD bypass DISABLED so we can debug the crash
# =============================================================================

# Patch-A: Memory allocator fix - device local preference
# Required for: BOTH (confirmed - FSR2 context fails without it)
set(PATCH_A_ENABLED ON)

# Patch-B: Memory allocator fix - dedicated allocation
# Required for: NEITHER (confirmed - not needed, Lavapipe faster without)
set(PATCH_B_ENABLED OFF)

# Patch-C: FP16 kill switch
# Required for: NEITHER (confirmed - makes no difference on any backend)
set(PATCH_C_ENABLED OFF)

# Patch-D: B10G11R11 format override
# Required for: NEITHER (confirmed - makes no difference on any backend)
set(PATCH_D_ENABLED OFF)

# Patch-E: SPD bypass
# Required for: BOTH (confirmed - fixes Lavapipe crash, fixes SwiftShader
#               silent corruption). DISABLED here for Phase 2 crash debug.
# Set ON to restore safe operation, OFF to expose crash for debugging.
set(PATCH_E_ENABLED OFF)

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
    GIT_TAG        master
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

FetchContent_Declare(ffx_sdk_v2
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
    GIT_TAG        v1.1.4
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
    # PATCH ffx_vk.cpp (Patches A, B, C, D)
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
        message(STATUS "[Patch-A] DISABLED by switch.")
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
        message(STATUS "[Patch-B] DISABLED by switch.")
    endif()

    if(PATCH_C_ENABLED)
        set(PATCH_C_SEARCH
            "deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16"
        )
        string(FIND "${vk_code}" "${PATCH_C_SEARCH}" PATCH_C_POS)
        if(PATCH_C_POS EQUAL -1)
            message(FATAL_ERROR "[Patch-C] FP16 kill switch search string not found!")
        else()
            string(REPLACE "${PATCH_C_SEARCH}"
                "deviceCapabilities->fp16Supported = false /* [Fix-C] FP16 kill switch */"
                vk_code "${vk_code}")
            message(STATUS "[Patch-C] Applied OK.")
        endif()
    else()
        message(STATUS "[Patch-C] DISABLED by switch.")
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
        message(STATUS "[Patch-D] DISABLED by switch.")
    endif()

    file(WRITE "${VK_BACKEND_FILE}" "${vk_code}")
    message(STATUS "[Patch] ffx_vk.cpp written.")

    # =========================================================================
    # PATCH ffx_fsr2.cpp - Patch E + Phase 2 SPD crash diagnostics
    # =========================================================================
    set(FSR2_CPP_FILE
        "${ffx_sdk_v2_SOURCE_DIR}/sdk/src/components/fsr2/ffx_fsr2.cpp"
    )
    if(NOT EXISTS "${FSR2_CPP_FILE}")
        message(FATAL_ERROR "[Patch-E] ffx_fsr2.cpp not found at: ${FSR2_CPP_FILE}")
    endif()
    file(READ "${FSR2_CPP_FILE}" fsr2_code)

    if(PATCH_E_ENABLED)
        # -------------------------------------------------------------------------
        # Patch-E ACTIVE: Skip SPD dispatch entirely
        # -------------------------------------------------------------------------
        set(PATCH_E_SEARCH
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        set(PATCH_E_REPLACE
            "/* [Fix-E] SPD dispatch SKIPPED - subgroupSize=4 incompatible with SPD */"
        )
        string(FIND "${fsr2_code}" "${PATCH_E_SEARCH}" PATCH_E_POS)
        if(PATCH_E_POS EQUAL -1)
            set(PATCH_E_SEARCH_ALT
                "&context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0]"
            )
            string(FIND "${fsr2_code}" "${PATCH_E_SEARCH_ALT}" PATCH_E_ALT_POS)
            if(PATCH_E_ALT_POS EQUAL -1)
                message(FATAL_ERROR "[Patch-E] SPD dispatch not found!")
            else()
                string(REPLACE "${PATCH_E_SEARCH_ALT}"
                    "&context->pipelineComputeLuminancePyramid, 0 /* [Fix-E] SPD bypassed */"
                    fsr2_code "${fsr2_code}")
                message(STATUS "[Patch-E] Applied via ALT search. OK!")
            endif()
        else()
            string(REPLACE "${PATCH_E_SEARCH}" "${PATCH_E_REPLACE}" fsr2_code "${fsr2_code}")
            message(STATUS "[Patch-E] SPD dispatch skipped. OK!")
        endif()

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")
        file(READ "${FSR2_CPP_FILE}" fsr2_verify)
        string(FIND "${fsr2_verify}" "Fix-E" VERIFY_E_POS)
        if(VERIFY_E_POS EQUAL -1)
            message(FATAL_ERROR "[VERIFY-E] Patch-E not confirmed in ffx_fsr2.cpp!")
        else()
            message(STATUS "[VERIFY-E] Patch-E confirmed. OK!")
        endif()

    else()
        # -------------------------------------------------------------------------
        # Patch-E INACTIVE: SPD runs normally.
        # Inject printf checkpoints around the entire luminance pyramid
        # setup block to pinpoint exactly where Lavapipe crashes.
        #
        # Strategy: wrap every significant operation with a before/after
        # printf so the last printed checkpoint before the crash tells us
        # exactly which operation caused it.
        #
        # From diagnostic output we know the relevant lines are:
        #   LINE 1643: Fsr2SpdConstants luminancePyramidConstants
        #   LINE 1645: luminancePyramidConstants.numworkGroups = ...
        #   LINE 1647: luminancePyramidConstants.mips = ...
        #   LINE 1649: luminancePyramidConstants.workGroupOffset[0] = ...
        #   LINE 1651: luminancePyramidConstants.workGroupOffset[1] = ...
        #   LINE 1653: luminancePyramidConstants.renderSize[0] = ...
        #   LINE 1655: luminancePyramidConstants.renderSize[1] = ...
        #   LINE 1684: &luminancePyramidConstants (passed to job)
        #   LINE 1710: scheduleDispatch(...)
        #
        # We inject checkpoints by wrapping the constants struct declaration
        # (first operation) and the scheduleDispatch call (last operation).
        # -------------------------------------------------------------------------
        message(STATUS "[Patch-E] DISABLED - injecting SPD crash diagnostics...")

        # Checkpoint 1: Before luminance pyramid constants setup
        # Search for the unique struct declaration line
        set(SPD_DIAG_SEARCH_1
            "Fsr2SpdConstants luminancePyramidConstants"
        )
        string(FIND "${fsr2_code}" "${SPD_DIAG_SEARCH_1}" SPD_DIAG_1_POS)
        if(SPD_DIAG_1_POS EQUAL -1)
            message(STATUS "[Phase2-Diag] WARNING: SPD constants declaration not found.")
        else()
            string(REPLACE "${SPD_DIAG_SEARCH_1}"
                "printf(\"[SPD-DIAG] Checkpoint 1: Before SPD constants setup\\n\"); fflush(stdout); Fsr2SpdConstants luminancePyramidConstants"
                fsr2_code "${fsr2_code}")
            message(STATUS "[Phase2-Diag] Checkpoint 1 injected before SPD constants.")
        endif()

        # Checkpoint 2: Before scheduleDispatch - print all SPD parameters
        # This is the last CPU operation before GPU work is submitted.
        # If we crash here, it is a CPU-side setup problem.
        # If we see this print but still crash, it is a GPU execution problem.
        set(SPD_DIAG_SEARCH_2
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        string(FIND "${fsr2_code}" "${SPD_DIAG_SEARCH_2}" SPD_DIAG_2_POS)
        if(SPD_DIAG_2_POS EQUAL -1)
            # Try alt search
            set(SPD_DIAG_SEARCH_2_ALT
                "&context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0]"
            )
            string(FIND "${fsr2_code}" "${SPD_DIAG_SEARCH_2_ALT}" SPD_DIAG_2_ALT_POS)
            if(SPD_DIAG_2_ALT_POS EQUAL -1)
                message(STATUS "[Phase2-Diag] WARNING: SPD scheduleDispatch not found.")
            else()
                string(REPLACE "${SPD_DIAG_SEARCH_2_ALT}"
                    "&context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0]"
                    fsr2_code "${fsr2_code}")
                message(STATUS "[Phase2-Diag] Checkpoint 2 found via ALT search.")
            endif()
        else()
            string(REPLACE "${SPD_DIAG_SEARCH_2}"
                "printf(\"[SPD-DIAG] Checkpoint 2: Before scheduleDispatch. Groups=[%u,%u]\\n\", dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]); fflush(stdout); scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
                fsr2_code "${fsr2_code}")
            message(STATUS "[Phase2-Diag] Checkpoint 2 injected before scheduleDispatch.")
        endif()

        # Checkpoint 3: After scheduleDispatch
        # We find the line AFTER scheduleDispatch by looking for what
        # follows it in the source. From the PDF the function ends
        # shortly after the dispatch with a return statement.
        # We inject after by appending to the replacement above.
        # Instead, we search for the return statement that closes
        # the luminance pyramid block.
        #
        # From ffx_fsr2.cpp in the PDF, after scheduleDispatch the
        # function returns FFX_OK. We find that return to add checkpoint 3.
        # We use a context-specific search to avoid matching other returns.
        set(SPD_DIAG_SEARCH_3
            "scheduleDispatch(context, params, &context->pipelineComputeLuminancePyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1])"
        )
        # Note: Checkpoint 3 is appended directly into the Checkpoint 2
        # replacement string above. If checkpoint 2 prints but 3 does not,
        # the crash is inside scheduleDispatch itself.
        # We add it as a separate injection after the dispatch call.
        # Search for the closing brace or return after the dispatch.

        file(WRITE "${FSR2_CPP_FILE}" "${fsr2_code}")

        # Verify diagnostics were injected
        file(READ "${FSR2_CPP_FILE}" fsr2_verify2)
        string(FIND "${fsr2_verify2}" "SPD-DIAG" SPD_DIAG_VERIFY_POS)
        if(SPD_DIAG_VERIFY_POS EQUAL -1)
            message(STATUS "[Phase2-Diag] WARNING: No SPD diagnostics confirmed in file.")
        else()
            message(STATUS "[Phase2-Diag] SPD crash diagnostics confirmed in ffx_fsr2.cpp. OK!")
        endif()

        message(STATUS "[Patch-E] DISABLED - SPD will run with diagnostics active.")
    endif()

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

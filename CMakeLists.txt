cmake_minimum_required(VERSION 3.19)

project(ParticleSaturnDiligent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd/IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# 输出目录：默认输出到 CMAKE_BINARY_DIR 根目录
# 既然父级构建脚本（targets/ps1）已经通过 -B 区分了配置（如 bin_diligent/x64/Release），
# 这里不再额外添加子目录，以保持路径简洁。
# ============================================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ============================================================================
# 添加 FastRelease 配置类型（快速迭代开发，无 PDB、无 LTCG）
# ============================================================================
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES FastRelease)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "" FORCE)
endif()

# FastRelease 基于 Release，但禁用 LTCG 和 PDB
set(CMAKE_CXX_FLAGS_FASTRELEASE "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_FASTRELEASE "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_FASTRELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_FASTRELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)
# 移除 /GL（LTCG 编译）如果存在
string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_FASTRELEASE "${CMAKE_CXX_FLAGS_FASTRELEASE}")
string(REPLACE "/GL" "" CMAKE_C_FLAGS_FASTRELEASE "${CMAKE_C_FLAGS_FASTRELEASE}")
# 移除 /LTCG（链接时代码生成）如果存在
string(REPLACE "/LTCG" "" CMAKE_EXE_LINKER_FLAGS_FASTRELEASE "${CMAKE_EXE_LINKER_FLAGS_FASTRELEASE}")
string(REPLACE "/LTCG" "" CMAKE_SHARED_LINKER_FLAGS_FASTRELEASE "${CMAKE_SHARED_LINKER_FLAGS_FASTRELEASE}")

# ============================================================================
# 版本号自动检测：git tag > CI commit hash > "dev"
# ============================================================================
find_package(Git QUIET)
set(APP_VERSION "dev")

if(GIT_FOUND)
    # 1. 检查是否有 tag 指向当前 commit
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_TAG_RESULT
    )
    if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG)
        set(APP_VERSION "${GIT_TAG}")
    else()
        # 2. 获取 commit hash
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_COMMIT_RESULT
        )
        if(GIT_COMMIT_RESULT EQUAL 0 AND GIT_COMMIT)
            # 检查是否在 CI 环境（常见 CI 环境变量）
            if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS} OR DEFINED ENV{GITLAB_CI} OR DEFINED ENV{JENKINS_URL})
                set(APP_VERSION "dev-${GIT_COMMIT}")
            else()
                # 本地开发：仍使用 "dev"，但可选显示 commit
                set(APP_VERSION "dev")
            endif()
        endif()
    endif()
endif()

message(STATUS "App version: ${APP_VERSION}")

# ============================================================================
# Build ID (用于着色器/PSO 缓存失效判定)
# - 与 APP_VERSION 不同：本地开发也会包含 git commit（避免长期 "dev" 导致缓存不失效）
# ============================================================================
set(APP_BUILD_ID "dev")
if(GIT_FOUND)
    if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG)
        set(APP_BUILD_ID "${GIT_TAG}")
    elseif(GIT_COMMIT_RESULT EQUAL 0 AND GIT_COMMIT)
        set(APP_BUILD_ID "${GIT_COMMIT}")
    endif()
endif()
message(STATUS "App build id: ${APP_BUILD_ID}")

include(CTest)
add_subdirectory(src/app)
add_subdirectory(src/gpu)
add_subdirectory(src/render)
add_subdirectory(src/shaders/abi)

add_library(ParticleSaturnSimd STATIC HandTracker/SIMDNormalize.cpp)
target_include_directories(ParticleSaturnSimd PUBLIC "${CMAKE_SOURCE_DIR}/HandTracker")
target_compile_features(ParticleSaturnSimd PUBLIC cxx_std_20)
if(BUILD_TESTING)
    add_executable(ParticleSaturnSimdTests tests/SIMDNormalizeTests.cpp)
    target_link_libraries(ParticleSaturnSimdTests PRIVATE ParticleSaturnSimd)
    add_test(NAME ParticleSaturnSimdTests COMMAND ParticleSaturnSimdTests)
endif()

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "" FORCE)
    add_subdirectory(src/shaders/msl)
    add_subdirectory(src/shaders/glsl410)
    add_subdirectory(src/gpu/backends/metal)
    add_subdirectory(src/gpu/backends/opengl41)
    add_subdirectory(src/platform/macos)
    add_subdirectory(src/services/settings/macos)
    add_subdirectory(src/services/camera/macos)
endif()

option(PARTICLESATURN_BUILD_LEGACY_DILIGENT "Build the Windows Diligent renderer" ${WIN32})

if(PARTICLESATURN_BUILD_LEGACY_DILIGENT AND NOT WIN32)
    message(FATAL_ERROR "ParticleSaturn.Diligent is a Windows-only legacy target")
endif()

if(NOT PARTICLESATURN_BUILD_LEGACY_DILIGENT)
    message(STATUS "Skipping Windows-only legacy Diligent renderer")
    return()
endif()

# ============================================================================
# DiligentCore 子模块版本（用于缓存失效判定）
# ============================================================================
set(DILIGENTCORE_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}/libs/DiligentCore" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE DILIGENTCORE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE DILIGENTCORE_COMMIT_RESULT
    )
    if(NOT (DILIGENTCORE_COMMIT_RESULT EQUAL 0) OR NOT DILIGENTCORE_COMMIT)
        set(DILIGENTCORE_COMMIT "unknown")
    endif()
endif()
message(STATUS "DiligentCore commit: ${DILIGENTCORE_COMMIT}")

# Provide a minimal ATL shim (CComPtr) only when needed.
#
# Rationale:
# - DiligentCore uses <atlbase.h>/<atlcomcli.h> for CComPtr on Win32.
# - On dev machines with ATL installed, we should prefer the real headers.
# - On CI/minimal VS installs, the shim can be enabled as a fallback.
option(PARTICLESATURN_FORCE_ATL_SHIM "Force using the local ATL shim headers" OFF)
option(PARTICLESATURN_FAST_BUILD "Prefer faster iteration builds (disables /GL for DiligentCore Release)" OFF)
set(PARTICLESATURN_HANDTRACKER_LINK_MODE "dll" CACHE STRING "HandTracker link mode: dll (runtime load) or static (link into exe)")
set_property(CACHE PARTICLESATURN_HANDTRACKER_LINK_MODE PROPERTY STRINGS dll static)

if(MSVC AND PARTICLESATURN_HANDTRACKER_LINK_MODE STREQUAL "static")
    # HandTracker Release_Static is built with /MT. Keep the entire CMake build consistent to avoid LNK2038 CRT mismatch.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

include(CheckIncludeFileCXX)
check_include_file_cxx(atlbase.h PARTICLESATURN_HAS_ATLBASE_H)
check_include_file_cxx(atlcomcli.h PARTICLESATURN_HAS_ATLCOMCLI_H)

if(PARTICLESATURN_FORCE_ATL_SHIM OR NOT (PARTICLESATURN_HAS_ATLBASE_H AND PARTICLESATURN_HAS_ATLCOMCLI_H))
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/shims/atl")
endif()

# Build D3D11 + D3D12 + Vulkan backends from DiligentCore.
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_DIRECT3D11 OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_METAL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_ARCHIVER OFF CACHE BOOL "" FORCE) # 启用 Archiver 以支持着色器缓存
set(DILIGENT_NO_HLSL ON CACHE BOOL "" FORCE) # We provide GLSL for Vulkan; keep HLSL use scoped to D3D12.

if(PARTICLESATURN_FAST_BUILD AND MSVC)
    # DiligentCore enables /GL in Release by default, which makes link time (LTCG) very slow.
    # For day-to-day iteration, disabling /GL speeds up incremental rebuilds significantly.
    set(DILIGENT_MSVC_RELEASE_COMPILE_OPTIONS "" CACHE STRING "Additional MSVC compile options for release configurations" FORCE)
endif()

# SPIRV-Tools enables /WX by default, which breaks with newer MSVC warnings (e.g. C5232).
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory(libs/DiligentCore)

if(MSVC)
    # DiligentCore's SPIRV-Tools submodule enables /WX unconditionally.
    # Newer MSVC emits warnings like C5232 (C++20), which then break the build.
    # We remove /WX from SPIRV-Tools targets instead of adding /WX- to avoid D9025 warning.
    foreach(_t IN ITEMS
        SPIRV-Tools-static
        SPIRV-Tools-shared
        SPIRV-Tools-opt
        SPIRV-Tools-link
        SPIRV-Tools-lint
        SPIRV-Tools-diff
        SPIRV-Tools-reduce
        SPIRV-Tools-fuzz
    )
        if(TARGET ${_t})
            get_target_property(_opts ${_t} COMPILE_OPTIONS)
            if(_opts)
                list(REMOVE_ITEM _opts "/WX")
                set_target_properties(${_t} PROPERTIES COMPILE_OPTIONS "${_opts}")
            endif()
        endif()
    endforeach()
endif()

# ============================================================================
# 着色器预编译：在构建前自动编译 HLSL/GLSL 到字节码
# ============================================================================
set(SHADER_COMPILE_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/compile_shaders.ps1")
set(SHADER_BYTECODES_HEADER "${CMAKE_SOURCE_DIR}/src/generated/ShaderBytecodes.h")

# 收集所有着色器源文件作为依赖
file(GLOB HLSL_SHADER_SOURCES "${CMAKE_SOURCE_DIR}/src/shaders/hlsl/*.hlsl")
file(GLOB GLSL_SHADER_SOURCES "${CMAKE_SOURCE_DIR}/src/shaders/glsl/*.glsl")

add_custom_command(
    OUTPUT "${SHADER_BYTECODES_HEADER}"
    COMMAND PowerShell -NoProfile -ExecutionPolicy Bypass -File "${SHADER_COMPILE_SCRIPT}"
    DEPENDS ${HLSL_SHADER_SOURCES} ${GLSL_SHADER_SOURCES} "${SHADER_COMPILE_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling shaders to bytecode..."
    VERBATIM
)

add_custom_target(CompileShaders
    DEPENDS "${SHADER_BYTECODES_HEADER}"
)

add_executable(ParticleSaturn.Diligent WIN32
    src/Diligent/Main.cpp
    src/Diligent/DiligentBackend.cpp
    src/Diligent/DiligentShaderSources.cpp
    src/Diligent/DirectCompositionSwapChain.cpp
    src/Diligent/VulkanD3D12Interop.cpp
    src/Diligent/ImGuiDiligent.cpp
    src/Diligent/HandTrackerController.cpp
    src/Diligent/Win32WindowManager.cpp
    src/Diligent/Settings.cpp

    # MD3 UI system
    src/Diligent/md3/MD3Theme.cpp
    src/Diligent/md3/MD3Context.cpp
    src/Diligent/md3/MD3Widgets.cpp

    # Camera selector (used by HandTracker)
    src/CameraSelector/CameraEnumerator.cpp
    src/CameraSelector/CameraPreview.cpp
    src/CameraSelector/CameraSelector.cpp
    src/CameraSelector/D2DRenderer.cpp

    # ImGui core (minimal set)
    libs/imgui/imgui.cpp
    libs/imgui/imgui_draw.cpp
    libs/imgui/imgui_tables.cpp
    libs/imgui/imgui_widgets.cpp

    # ImGui Win32 platform backend
    libs/imgui/backends/imgui_impl_win32.cpp

    # Windows manifest for DPI awareness (PerMonitorV2)
    app.manifest
)

target_include_directories(ParticleSaturn.Diligent PRIVATE
    src/Diligent
    src/Diligent/md3
    src
    HandTracker/include
    libs/imgui
    libs/imgui/backends
    # For VulkanD3D12Interop: access to VulkanHeaders.h and volk
    libs/DiligentCore/Graphics/GraphicsEngineVulkan/include
    libs/DiligentCore/ThirdParty/Vulkan-Headers/include
    libs/DiligentCore/ThirdParty/volk
)

# Link all three backends so a single build can switch at runtime.
target_link_libraries(ParticleSaturn.Diligent PRIVATE
    Diligent-GraphicsEngine
    Diligent-GraphicsEngineD3D11-static
    Diligent-GraphicsEngineD3D12-static
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsTools    # For RenderStateCache
    Diligent-Archiver-static  # For shader caching
    dwmapi
    dcomp
    d3d11
    dxgi
)

# 确保着色器在编译 C++ 代码之前已预编译
add_dependencies(ParticleSaturn.Diligent CompileShaders)

if(PARTICLESATURN_HANDTRACKER_LINK_MODE STREQUAL "static")
    target_compile_definitions(ParticleSaturn.Diligent PRIVATE HANDTRACKER_STATIC)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_ht_platform "x64")
    else()
        set(_ht_platform "Win32")
    endif()

    set(_ht_lib_dir "${CMAKE_SOURCE_DIR}/bin/${_ht_platform}/Release_Static")
    set(_opencv_install_root "${CMAKE_SOURCE_DIR}/HandTracker/libs/opencv/build/install/${_ht_platform}")

    file(GLOB _opencv_staticlib_candidates LIST_DIRECTORIES true "${_opencv_install_root}/vc*/staticlib")
    list(SORT _opencv_staticlib_candidates)
    list(LENGTH _opencv_staticlib_candidates _opencv_candidates_len)
    if(_opencv_candidates_len GREATER 0)
        list(GET _opencv_staticlib_candidates 0 _opencv_staticlib_dir)
    else()
        set(_opencv_staticlib_dir "${_opencv_install_root}/vc17/staticlib")
    endif()

    target_link_directories(ParticleSaturn.Diligent PRIVATE
        "${_ht_lib_dir}"
        "${_opencv_staticlib_dir}"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/install/lib"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/protobuf-build/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/strings/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/base/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/numeric/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/container/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/synchronization/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/time/MinSizeRel"
        "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow/tflite_build/_deps/abseil-cpp-build/absl/debugging/MinSizeRel"
    )

    # Mirror the OpenGL Release_Static link set so the static HandTracker build actually works.
    target_link_libraries(ParticleSaturn.Diligent PRIVATE
        HandTracker.lib
        opencv_world4130.lib
        libprotobuf.lib
        libjpeg-turbo.lib
        libpng.lib
        zlib.lib
        tensorflow-lite.lib
        xnnpack-delegate.lib
        XNNPACK.lib
        cpuinfo.lib
        pthreadpool.lib
        farmhash.lib
        fft2d_fftsg.lib
        fft2d_fftsg2d.lib
        flatbuffers.lib
        eight_bit_int_gemm.lib
        ruy_frontend.lib
        ruy_trmul.lib
        ruy_prepare_packed_matrices.lib
        ruy_ctx.lib
        ruy_context.lib
        ruy_context_get_ctx.lib
        ruy_allocator.lib
        ruy_system_aligned_alloc.lib
        ruy_apply_multiplier.lib
        ruy_block_map.lib
        ruy_blocking_counter.lib
        ruy_cpuinfo.lib
        ruy_denormal.lib
        ruy_have_built_path_for_avx.lib
        ruy_have_built_path_for_avx2_fma.lib
        ruy_have_built_path_for_avx512.lib
        ruy_kernel_arm.lib
        ruy_kernel_avx.lib
        ruy_kernel_avx2_fma.lib
        ruy_kernel_avx512.lib
        ruy_pack_arm.lib
        ruy_pack_avx.lib
        ruy_pack_avx2_fma.lib
        ruy_pack_avx512.lib
        ruy_prepacked_cache.lib
        ruy_profiler_instrumentation.lib
        ruy_thread_pool.lib
        ruy_tune.lib
        ruy_wait.lib
        microkernels-prod.lib
        absl_strings.lib
        absl_strings_internal.lib
        absl_string_view.lib
        absl_base.lib
        absl_throw_delegate.lib
        absl_raw_logging_internal.lib
        absl_log_severity.lib
        absl_spinlock_wait.lib
        absl_int128.lib
        absl_hashtablez_sampler.lib
        absl_raw_hash_set.lib
        absl_synchronization.lib
        absl_graphcycles_internal.lib
        absl_kernel_timeout_internal.lib
        absl_time.lib
        absl_time_zone.lib
        absl_civil_time.lib
        absl_malloc_internal.lib
        absl_stacktrace.lib
        absl_symbolize.lib
        absl_debugging_internal.lib
        absl_demangle_internal.lib

        # Media Foundation + misc Win32 deps used by HandTracker/OpenCV.
        kernel32.lib
        user32.lib
        gdi32.lib
        winspool.lib
        comdlg32.lib
        advapi32.lib
        shell32.lib
        ole32.lib
        oleaut32.lib
        uuid.lib
        odbc32.lib
        odbccp32.lib
        mf.lib
        mfplat.lib
        mfreadwrite.lib
        mfuuid.lib
        shlwapi.lib
    )
endif()

target_compile_definitions(ParticleSaturn.Diligent PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    # 启用 volk（Vulkan 函数加载器），与 DiligentCore 保持一致
    DILIGENT_USE_VOLK=1
    # 使用 Diligent 版的 imconfig.h（与 OpenGL 版分开）
    IMGUI_USER_CONFIG="imconfig.h"
    # 版本号（用于崩溃报告等）
    APP_VERSION="${APP_VERSION}"
    # 构建标识（用于着色器/PSO 缓存失效判定：本地开发也会包含 commit）
    APP_BUILD_ID="${APP_BUILD_ID}"
    # DiligentCore 子模块版本（用于着色器/PSO 缓存失效判定）
    DILIGENTCORE_COMMIT="${DILIGENTCORE_COMMIT}"
)

if(MSVC)
    # Ensure UTF-8 source decoding so non-ASCII literals in app code compile reliably.
    target_compile_options(ParticleSaturn.Diligent PRIVATE /utf-8)

    # Release 配置：生成 PDB 用于事后崩溃分析，但不泄露开发环境路径
    # /LTCG 启用链接时代码生成，与 DiligentCore 的 /GL 编译兼容
    target_link_options(ParticleSaturn.Diligent PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>
        $<$<CONFIG:Release>:/LTCG>
    )

    # FastRelease 配置：不生成 PDB，最快编译速度
    target_link_options(ParticleSaturn.Diligent PRIVATE
        $<$<CONFIG:FastRelease>:/DEBUG:NONE>
    )
endif()

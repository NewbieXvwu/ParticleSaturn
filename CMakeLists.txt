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

# Provide a minimal ATL shim (CComPtr) only when needed.
#
# Rationale:
# - DiligentCore uses <atlbase.h>/<atlcomcli.h> for CComPtr on Win32.
# - On dev machines with ATL installed, we should prefer the real headers.
# - On CI/minimal VS installs, the shim can be enabled as a fallback.
option(PARTICLESATURN_FORCE_ATL_SHIM "Force using the local ATL shim headers" OFF)
option(PARTICLESATURN_FAST_BUILD "Prefer faster iteration builds (disables /GL for DiligentCore Release)" OFF)

include(CheckIncludeFileCXX)
check_include_file_cxx(atlbase.h PARTICLESATURN_HAS_ATLBASE_H)
check_include_file_cxx(atlcomcli.h PARTICLESATURN_HAS_ATLCOMCLI_H)

if(PARTICLESATURN_FORCE_ATL_SHIM OR NOT (PARTICLESATURN_HAS_ATLBASE_H AND PARTICLESATURN_HAS_ATLCOMCLI_H))
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/shims/atl")
endif()

# Build only what we need from DiligentCore for now (Vulkan + D3D12).
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_METAL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_ARCHIVER ON CACHE BOOL "" FORCE)
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

add_executable(ParticleSaturn.Diligent WIN32
    src/Diligent/Main.cpp
    src/Diligent/DiligentBackend.cpp
    src/Diligent/ImGuiDiligent.cpp
    src/Diligent/Win32WindowManager.cpp

    # MD3 UI system
    src/Diligent/md3/MD3Theme.cpp
    src/Diligent/md3/MD3Context.cpp
    src/Diligent/md3/MD3Widgets.cpp

    # ImGui core (minimal set)
    libs/imgui/imgui.cpp
    libs/imgui/imgui_draw.cpp
    libs/imgui/imgui_tables.cpp
    libs/imgui/imgui_widgets.cpp

    # ImGui Win32 platform backend
    libs/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(ParticleSaturn.Diligent PRIVATE
    src/Diligent
    src/Diligent/md3
    src
    libs/imgui
    libs/imgui/backends
)

# Link both backends so a single build can switch at runtime.
target_link_libraries(ParticleSaturn.Diligent PRIVATE
    Diligent-GraphicsEngine
    Diligent-GraphicsEngineD3D12-static
    Diligent-GraphicsEngineVk-static
    dwmapi
)

target_compile_definitions(ParticleSaturn.Diligent PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    # 使用 Diligent 版的 imconfig.h（与 OpenGL 版分开）
    IMGUI_USER_CONFIG="imconfig.h"
    # 版本号（用于崩溃报告等）
    APP_VERSION="${APP_VERSION}"
)

if(MSVC)
    # Ensure UTF-8 source decoding so non-ASCII literals in app code compile reliably.
    target_compile_options(ParticleSaturn.Diligent PRIVATE /utf-8)

    # Release 配置：生成 PDB 用于事后崩溃分析，但不泄露开发环境路径
    target_link_options(ParticleSaturn.Diligent PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>
    )

    # FastRelease 配置：不生成 PDB，最快编译速度
    target_link_options(ParticleSaturn.Diligent PRIVATE
        $<$<CONFIG:FastRelease>:/DEBUG:NONE>
    )
endif()

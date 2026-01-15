cmake_minimum_required(VERSION 3.19)

project(ParticleSaturnDiligent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd/IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
)

if(MSVC)
    # Ensure UTF-8 source decoding so non-ASCII literals in app code compile reliably.
    target_compile_options(ParticleSaturn.Diligent PRIVATE /utf-8)
endif()

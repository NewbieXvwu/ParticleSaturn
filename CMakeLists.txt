cmake_minimum_required(VERSION 3.19)

project(ParticleSaturnDiligent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Provide a minimal ATL shim (CComPtr) only when needed.
#
# Rationale:
# - DiligentCore uses <atlbase.h>/<atlcomcli.h> for CComPtr on Win32.
# - On dev machines with ATL installed, we should prefer the real headers.
# - On CI/minimal VS installs, the shim can be enabled as a fallback.
option(PARTICLESATURN_FORCE_ATL_SHIM "Force using the local ATL shim headers" OFF)

include(CheckIncludeFileCXX)
check_include_file_cxx(atlbase.h PARTICLESATURN_HAS_ATLBASE_H)
check_include_file_cxx(atlcomcli.h PARTICLESATURN_HAS_ATLCOMCLI_H)

if(PARTICLESATURN_FORCE_ATL_SHIM OR NOT (PARTICLESATURN_HAS_ATLBASE_H AND PARTICLESATURN_HAS_ATLCOMCLI_H))
    include_directories(BEFORE "${CMAKE_SOURCE_DIR}/shims/atl")
endif()

# Build only what we need from DiligentCore for now (Vulkan + D3D12).
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_METAL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_ARCHIVER ON CACHE BOOL "" FORCE)

# SPIRV-Tools enables /WX by default, which breaks with newer MSVC warnings (e.g. C5232).
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory(libs/DiligentCore)

if(MSVC)
    # DiligentCore's SPIRV-Tools submodule enables /WX unconditionally.
    # Newer MSVC emits warnings like C5232 (C++20), which then break the build.
    # We only neutralize /WX for SPIRV-Tools targets to keep the rest of the build strict.
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
            target_compile_options(${_t} PRIVATE /WX-)
        endif()
    endforeach()
endif()

add_executable(ParticleSaturn.Diligent WIN32
    src/Diligent/Main.cpp
    src/Diligent/DiligentBackend.cpp
)

target_include_directories(ParticleSaturn.Diligent PRIVATE
    src/Diligent
    src
)

# Link both backends so a single build can switch at runtime.
target_link_libraries(ParticleSaturn.Diligent PRIVATE
    Diligent-GraphicsEngine
    Diligent-GraphicsEngineD3D12-static
    Diligent-GraphicsEngineVk-static
)

target_compile_definitions(ParticleSaturn.Diligent PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

if(MSVC)
    # Ensure UTF-8 source decoding so non-ASCII literals in app code compile reliably.
    target_compile_options(ParticleSaturn.Diligent PRIVATE /utf-8)
endif()

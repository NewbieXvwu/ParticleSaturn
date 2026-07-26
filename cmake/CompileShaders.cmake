# CompileShaders.cmake
# Cross-platform shader compilation script
# Compiles HLSL -> DXBC/DXIL, GLSL -> SPIR-V and generates C++ byte arrays
#
# Required variables:
#   SHADER_SRC_DIR     - Directory containing shader source files
#   ABI_OUTPUT_DIR     - Directory containing generated ABI headers
#   OUTPUT_HEADER      - Output header file path
#   ABI_GENERATOR      - Path to GenerateShaderAbi.cmake
#   ABI_INPUT          - Path to particle_abi.json

cmake_minimum_required(VERSION 3.20)

# Ensure output directory exists
get_filename_component(OUTPUT_DIR "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# Generate ABI first
message(STATUS "Generating particle ABI...")
execute_process(
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${ABI_INPUT}"
        "-DOUTPUT_DIRECTORY=${ABI_OUTPUT_DIR}"
        -P "${ABI_GENERATOR}"
    RESULT_VARIABLE ABI_RESULT
)

if(NOT ABI_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate particle ABI")
endif()

# ============================================================================
# Find shader compilers
# ============================================================================

function(find_dxc OUT_VAR)
    if(WIN32)
        # Try Windows SDK
        file(GLOB_RECURSE DXC_PATHS
            "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
        )
        if(DXC_PATHS)
            list(SORT DXC_PATHS)
            list(REVERSE DXC_PATHS)
            list(GET DXC_PATHS 0 DXC_PATH)
            set(${OUT_VAR} "${DXC_PATH}" PARENT_SCOPE)
            return()
        endif()

        # Try Visual Studio
        file(GLOB_RECURSE DXC_PATHS
            "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dxc.exe"
            "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dxc.exe"
        )
        if(DXC_PATHS)
            list(SORT DXC_PATHS)
            list(REVERSE DXC_PATHS)
            list(GET DXC_PATHS 0 DXC_PATH)
            set(${OUT_VAR} "${DXC_PATH}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Try PATH
    find_program(DXC_FOUND dxc)
    if(DXC_FOUND)
        set(${OUT_VAR} "${DXC_FOUND}" PARENT_SCOPE)
        return()
    endif()

    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

function(find_fxc OUT_VAR)
    if(WIN32)
        file(GLOB_RECURSE FXC_PATHS
            "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe"
        )
        if(FXC_PATHS)
            list(SORT FXC_PATHS)
            list(REVERSE FXC_PATHS)
            list(GET FXC_PATHS 0 FXC_PATH)
            set(${OUT_VAR} "${FXC_PATH}" PARENT_SCOPE)
            return()
        endif()
    endif()

    find_program(FXC_FOUND fxc)
    if(FXC_FOUND)
        set(${OUT_VAR} "${FXC_FOUND}" PARENT_SCOPE)
        return()
    endif()

    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

function(find_glslang OUT_VAR)
    # Try VULKAN_SDK environment variable
    if(DEFINED ENV{VULKAN_SDK})
        if(WIN32)
            set(GLSLANG_PATH "$ENV{VULKAN_SDK}/Bin/glslangValidator.exe")
        else()
            set(GLSLANG_PATH "$ENV{VULKAN_SDK}/bin/glslangValidator")
        endif()
        if(EXISTS "${GLSLANG_PATH}")
            set(${OUT_VAR} "${GLSLANG_PATH}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Try PATH
    find_program(GLSLANG_FOUND glslangValidator)
    if(GLSLANG_FOUND)
        set(${OUT_VAR} "${GLSLANG_FOUND}" PARENT_SCOPE)
        return()
    endif()

    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

find_dxc(DXC)
find_fxc(FXC)
find_glslang(GLSLANG)

message(STATUS "Shader compilers:")
message(STATUS "  DXC: ${DXC}")
message(STATUS "  FXC: ${FXC}")
message(STATUS "  glslangValidator: ${GLSLANG}")

if(NOT DXC AND NOT FXC)
    message(FATAL_ERROR "Neither DXC nor FXC found. Cannot compile HLSL shaders.")
endif()

if(NOT GLSLANG)
    message(WARNING "glslangValidator not found. Vulkan shaders will not be pre-compiled.")
endif()

# ============================================================================
# Shader definitions
# ============================================================================

set(SHADERS
    # Format: Name;HasVS;HasPS;HasCS;HasMS;HasHLSL;HasGLSL
    "FullscreenQuad;1;1;0;0;1;1"
    "BloomDownsample;1;1;0;0;1;1"
    "BloomBlur;1;1;0;0;1;1"
    "AcrylicComposite;1;1;0;0;1;1"
    "Star;1;1;0;0;1;1"
    "SaturnParticle;1;1;0;0;1;1"
    "SevenSeg;1;1;0;0;1;1"
    "SaturnCompute;0;0;1;0;1;1"
    "SaturnInit;0;0;1;0;1;1"
    "SaturnParticleMesh;0;1;0;1;1;0"
)

# ============================================================================
# Compilation functions
# ============================================================================

function(compile_hlsl INPUT_FILE OUTPUT_FILE ENTRY_POINT PROFILE)
    set(COMPILER "")
    set(ARGS "")

    # Use DXC for SM 6.x (mesh shaders), FXC for SM 5.x (D3D11 compatibility)
    if(PROFILE MATCHES "^[a-z]+_6_")
        if(NOT DXC)
            message(WARNING "DXC required for ${PROFILE} but not found, skipping ${INPUT_FILE}")
            set(COMPILE_SUCCESS FALSE PARENT_SCOPE)
            return()
        endif()
        set(COMPILER "${DXC}")
        set(ARGS -T "${PROFILE}" -E "${ENTRY_POINT}" -Fo "${OUTPUT_FILE}"
                 -I "${ABI_OUTPUT_DIR}" "${INPUT_FILE}" -O3)
    else()
        # SM 5.x: prefer FXC for D3D11 compatibility
        if(FXC)
            set(COMPILER "${FXC}")
            set(ARGS /T "${PROFILE}" /E "${ENTRY_POINT}" /Fo "${OUTPUT_FILE}"
                     /I "${ABI_OUTPUT_DIR}" /O3 "${INPUT_FILE}")
        elseif(DXC)
            set(COMPILER "${DXC}")
            set(ARGS -T "${PROFILE}" -E "${ENTRY_POINT}" -Fo "${OUTPUT_FILE}"
                     -I "${ABI_OUTPUT_DIR}" "${INPUT_FILE}" -O3)
        else()
            message(WARNING "No HLSL compiler found for ${INPUT_FILE}")
            set(COMPILE_SUCCESS FALSE PARENT_SCOPE)
            return()
        endif()
    endif()

    execute_process(
        COMMAND ${COMPILER} ${ARGS}
        RESULT_VARIABLE RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(RESULT EQUAL 0)
        set(COMPILE_SUCCESS TRUE PARENT_SCOPE)
    else()
        message(WARNING "Failed to compile ${INPUT_FILE}")
        set(COMPILE_SUCCESS FALSE PARENT_SCOPE)
    endif()
endfunction()

function(compile_glsl INPUT_FILE OUTPUT_FILE STAGE)
    if(NOT GLSLANG)
        set(COMPILE_SUCCESS FALSE PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND ${GLSLANG} -V -S "${STAGE}" "-I${ABI_OUTPUT_DIR}"
                -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(RESULT EQUAL 0)
        set(COMPILE_SUCCESS TRUE PARENT_SCOPE)
    else()
        message(WARNING "Failed to compile ${INPUT_FILE}")
        set(COMPILE_SUCCESS FALSE PARENT_SCOPE)
    endif()
endfunction()

function(convert_to_byte_array BINARY_FILE ARRAY_NAME OUT_VAR)
    if(NOT EXISTS "${BINARY_FILE}")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    file(READ "${BINARY_FILE}" BINARY_DATA HEX)

    string(LENGTH "${BINARY_DATA}" DATA_LEN)
    math(EXPR BYTE_COUNT "${DATA_LEN} / 2")

    set(RESULT "alignas(4) constexpr unsigned char ${ARRAY_NAME}[] = {\n")

    set(OFFSET 0)
    while(OFFSET LESS DATA_LEN)
        string(APPEND RESULT "    ")

        set(LINE_END ${OFFSET})
        math(EXPR LINE_END "${LINE_END} + 32")
        if(LINE_END GREATER DATA_LEN)
            set(LINE_END ${DATA_LEN})
        endif()

        while(OFFSET LESS LINE_END)
            string(SUBSTRING "${BINARY_DATA}" ${OFFSET} 2 BYTE_HEX)
            string(APPEND RESULT "0x${BYTE_HEX}")

            math(EXPR OFFSET "${OFFSET} + 2")
            if(OFFSET LESS DATA_LEN)
                string(APPEND RESULT ", ")
            endif()
        endwhile()

        string(APPEND RESULT "\n")
    endwhile()

    string(APPEND RESULT "};\n")
    set(${OUT_VAR} "${RESULT}" PARENT_SCOPE)
endfunction()

# ============================================================================
# Main compilation loop
# ============================================================================

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shader_temp")
set(TEMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/shader_temp")

set(HEADER_CONTENT "// Auto-generated shader bytecodes - DO NOT EDIT\n")
string(APPEND HEADER_CONTENT "// Generated by cmake/CompileShaders.cmake\n")
string(APPEND HEADER_CONTENT "#pragma once\n\n")
string(APPEND HEADER_CONTENT "#include <cstddef>\n\n")
string(APPEND HEADER_CONTENT "namespace ParticleSaturn::ShaderBytecodes {\n\n")

set(SUCCESS_COUNT 0)
set(FAIL_COUNT 0)
set(SKIP_COUNT 0)

foreach(SHADER_DEF ${SHADERS})
    string(REPLACE ";" ";" SHADER_PARTS "${SHADER_DEF}")
    list(GET SHADER_PARTS 0 NAME)
    list(GET SHADER_PARTS 1 HAS_VS)
    list(GET SHADER_PARTS 2 HAS_PS)
    list(GET SHADER_PARTS 3 HAS_CS)
    list(GET SHADER_PARTS 4 HAS_MS)
    list(GET SHADER_PARTS 5 HAS_HLSL)
    list(GET SHADER_PARTS 6 HAS_GLSL)

    message(STATUS "Compiling ${NAME}...")

    set(HLSL_DIR "${SHADER_SRC_DIR}/hlsl")
    set(GLSL_DIR "${SHADER_SRC_DIR}/glsl")

    # HLSL Vertex Shader
    if(HAS_VS AND HAS_HLSL)
        set(SRC "${HLSL_DIR}/${NAME}_VS.hlsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_VS.dxbc")
            compile_hlsl("${SRC}" "${OUT}" "main" "vs_5_0")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_VS_DXBC" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()

    # HLSL Pixel Shader
    if(HAS_PS AND HAS_HLSL)
        set(SRC "${HLSL_DIR}/${NAME}_PS.hlsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_PS.dxbc")
            compile_hlsl("${SRC}" "${OUT}" "main" "ps_5_0")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_PS_DXBC" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()

    # HLSL Compute Shader
    if(HAS_CS AND HAS_HLSL)
        set(SRC "${HLSL_DIR}/${NAME}_CS.hlsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_CS.dxbc")
            compile_hlsl("${SRC}" "${OUT}" "main" "cs_5_0")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_CS_DXBC" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()

    # HLSL Mesh Shader (SM 6.5)
    if(HAS_MS AND HAS_HLSL)
        set(SRC "${HLSL_DIR}/${NAME}_MS.hlsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_MS.dxil")
            compile_hlsl("${SRC}" "${OUT}" "main" "ms_6_5")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_MS_DXIL" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()

        # Also compile PS for mesh shader PSO with SM 6.5
        set(SRC_PS "${HLSL_DIR}/${NAME}_PS.hlsl")
        if(EXISTS "${SRC_PS}")
            set(OUT_PS "${TEMP_DIR}/${NAME}_MeshPS.dxil")
            compile_hlsl("${SRC_PS}" "${OUT_PS}" "main" "ps_6_5")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT_PS}" "${NAME}_MeshPS_DXIL" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            endif()
        endif()
    endif()

    # GLSL Vertex Shader -> SPIR-V
    if(HAS_VS AND HAS_GLSL AND GLSLANG)
        set(SRC "${GLSL_DIR}/${NAME}_VS.glsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_VS.spv")
            compile_glsl("${SRC}" "${OUT}" "vert")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_VS_SPIRV" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()

    # GLSL Fragment Shader -> SPIR-V
    if(HAS_PS AND HAS_GLSL AND GLSLANG)
        set(SRC "${GLSL_DIR}/${NAME}_PS.glsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_PS.spv")
            compile_glsl("${SRC}" "${OUT}" "frag")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_PS_SPIRV" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()

    # GLSL Compute Shader -> SPIR-V
    if(HAS_CS AND HAS_GLSL AND GLSLANG)
        set(SRC "${GLSL_DIR}/${NAME}_CS.glsl")
        if(EXISTS "${SRC}")
            set(OUT "${TEMP_DIR}/${NAME}_CS.spv")
            compile_glsl("${SRC}" "${OUT}" "comp")
            if(COMPILE_SUCCESS)
                convert_to_byte_array("${OUT}" "${NAME}_CS_SPIRV" BYTES)
                if(BYTES)
                    string(APPEND HEADER_CONTENT "${BYTES}\n")
                    math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
                endif()
            else()
                math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
            endif()
        else()
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
        endif()
    endif()
endforeach()

string(APPEND HEADER_CONTENT "} // namespace ParticleSaturn::ShaderBytecodes\n")

# Write output header
file(WRITE "${OUTPUT_HEADER}" "${HEADER_CONTENT}")

message(STATUS "")
message(STATUS "Shader compilation complete:")
message(STATUS "  Success: ${SUCCESS_COUNT}")
message(STATUS "  Failed:  ${FAIL_COUNT}")
message(STATUS "  Skipped: ${SKIP_COUNT}")
message(STATUS "  Output:  ${OUTPUT_HEADER}")

# Cleanup temp files
file(REMOVE_RECURSE "${TEMP_DIR}")

if(FAIL_COUNT GREATER 0)
    message(FATAL_ERROR "Shader compilation failed")
endif()

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
    # Format: Name|HasVS|HasPS|HasCS|HasMS|HasHLSL|HasGLSL
    # 字段分隔符用 '|'：CMake 会把带 ';' 的字符串拍平成一维列表，
    # 导致 foreach 只拿到单个 token、list(GET ... 1) 越界。
    "FullscreenQuad|1|1|0|0|1|1"
    "BloomDownsample|1|1|0|0|1|1"
    "BloomBlur|1|1|0|0|1|1"
    "AcrylicComposite|1|1|0|0|1|1"
    "Star|1|1|0|0|1|1"
    "SaturnParticle|1|1|0|0|1|1"
    "SevenSeg|1|1|0|0|1|1"
    "SaturnCompute|0|0|1|0|1|1"
    "SaturnInit|0|0|1|0|1|1"
    "SaturnParticleMesh|0|1|0|1|1|0"
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

    set(RESULT "alignas(4) constexpr unsigned char ${ARRAY_NAME}[] = {\n")

    # 按 16 字节（32 个 hex 字符）一行分块；每行用单次 REGEX 把 "NN" 成型为
    # "0xNN, "，取代逐字节 substring 循环。字节值与旧实现逐位一致（file READ HEX
    # 给出小写 hex），仅格式化方式改为正则。
    set(OFFSET 0)
    while(OFFSET LESS DATA_LEN)
        math(EXPR REMAIN "${DATA_LEN} - ${OFFSET}")
        if(REMAIN GREATER 32)
            set(CHUNK_LEN 32)
        else()
            set(CHUNK_LEN ${REMAIN})
        endif()
        string(SUBSTRING "${BINARY_DATA}" ${OFFSET} ${CHUNK_LEN} CHUNK)
        string(REGEX REPLACE "(..)" "0x\\1, " LINE "${CHUNK}")
        string(APPEND RESULT "    ${LINE}\n")
        math(EXPR OFFSET "${OFFSET} + 32")
    endwhile()

    # 去掉整个数组末尾多余的 ", "（末行行尾），保持与旧实现相同的收尾形态。
    string(REGEX REPLACE ", \n$" "\n" RESULT "${RESULT}")
    string(APPEND RESULT "};\n")
    set(${OUT_VAR} "${RESULT}" PARENT_SCOPE)
endfunction()

# ============================================================================
# 单一 stage 编译入口：取代旧版 8 段近乎复制的 per-stage 代码块。
# compile_stage(NAME LANG STAGE)
#   LANG  = HLSL | GLSL
#   STAGE = VS | PS | CS | MS | MeshPS（MeshPS 仅 HLSL）
# 就地累加 HEADER_CONTENT / SUCCESS_COUNT / FAIL_COUNT / SKIP_COUNT（PARENT_SCOPE）。
# 计数语义与旧版逐段一致：常规段缺源 +SKIP、编译失败 +FAIL；MeshPS 为尽力而为
# （缺源不 +SKIP、失败不 +FAIL）；GLSL 段仅在 glslang 存在时尝试，缺则静默跳过不计数。
# ============================================================================
function(compile_stage NAME LANG STAGE)
    set(STRICT TRUE)

    if(LANG STREQUAL "HLSL")
        if(STAGE STREQUAL "VS")
            set(SRC "${HLSL_DIR}/${NAME}_VS.hlsl")
            set(OUT "${TEMP_DIR}/${NAME}_VS.dxbc")
            set(ARRAY "${NAME}_VS_DXBC")
            set(PROFILE "vs_5_0")
        elseif(STAGE STREQUAL "PS")
            set(SRC "${HLSL_DIR}/${NAME}_PS.hlsl")
            set(OUT "${TEMP_DIR}/${NAME}_PS.dxbc")
            set(ARRAY "${NAME}_PS_DXBC")
            set(PROFILE "ps_5_0")
        elseif(STAGE STREQUAL "CS")
            set(SRC "${HLSL_DIR}/${NAME}_CS.hlsl")
            set(OUT "${TEMP_DIR}/${NAME}_CS.dxbc")
            set(ARRAY "${NAME}_CS_DXBC")
            set(PROFILE "cs_5_0")
        elseif(STAGE STREQUAL "MS")
            set(SRC "${HLSL_DIR}/${NAME}_MS.hlsl")
            set(OUT "${TEMP_DIR}/${NAME}_MS.dxil")
            set(ARRAY "${NAME}_MS_DXIL")
            set(PROFILE "ms_6_5")
        elseif(STAGE STREQUAL "MeshPS")
            # 网格管线用的 PS（SM 6.5），尽力而为：源自同名 _PS.hlsl。
            set(SRC "${HLSL_DIR}/${NAME}_PS.hlsl")
            set(OUT "${TEMP_DIR}/${NAME}_MeshPS.dxil")
            set(ARRAY "${NAME}_MeshPS_DXIL")
            set(PROFILE "ps_6_5")
            set(STRICT FALSE)
        else()
            message(FATAL_ERROR "compile_stage: unknown HLSL stage ${STAGE}")
        endif()

        if(NOT EXISTS "${SRC}")
            if(STRICT)
                math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
                set(SKIP_COUNT ${SKIP_COUNT} PARENT_SCOPE)
            endif()
            return()
        endif()

        compile_hlsl("${SRC}" "${OUT}" "main" "${PROFILE}")
    elseif(LANG STREQUAL "GLSL")
        if(NOT GLSLANG)
            return()
        endif()

        if(STAGE STREQUAL "VS")
            set(SRC "${GLSL_DIR}/${NAME}_VS.glsl")
            set(OUT "${TEMP_DIR}/${NAME}_VS.spv")
            set(ARRAY "${NAME}_VS_SPIRV")
            set(GLSTAGE "vert")
        elseif(STAGE STREQUAL "PS")
            set(SRC "${GLSL_DIR}/${NAME}_PS.glsl")
            set(OUT "${TEMP_DIR}/${NAME}_PS.spv")
            set(ARRAY "${NAME}_PS_SPIRV")
            set(GLSTAGE "frag")
        elseif(STAGE STREQUAL "CS")
            set(SRC "${GLSL_DIR}/${NAME}_CS.glsl")
            set(OUT "${TEMP_DIR}/${NAME}_CS.spv")
            set(ARRAY "${NAME}_CS_SPIRV")
            set(GLSTAGE "comp")
        else()
            message(FATAL_ERROR "compile_stage: unknown GLSL stage ${STAGE}")
        endif()

        if(NOT EXISTS "${SRC}")
            math(EXPR SKIP_COUNT "${SKIP_COUNT} + 1")
            set(SKIP_COUNT ${SKIP_COUNT} PARENT_SCOPE)
            return()
        endif()

        compile_glsl("${SRC}" "${OUT}" "${GLSTAGE}")
    else()
        message(FATAL_ERROR "compile_stage: unknown language ${LANG}")
    endif()

    if(COMPILE_SUCCESS)
        convert_to_byte_array("${OUT}" "${ARRAY}" BYTES)
        if(BYTES)
            string(APPEND HEADER_CONTENT "${BYTES}\n")
            set(HEADER_CONTENT "${HEADER_CONTENT}" PARENT_SCOPE)
            math(EXPR SUCCESS_COUNT "${SUCCESS_COUNT} + 1")
            set(SUCCESS_COUNT ${SUCCESS_COUNT} PARENT_SCOPE)
        endif()
    elseif(STRICT)
        math(EXPR FAIL_COUNT "${FAIL_COUNT} + 1")
        set(FAIL_COUNT ${FAIL_COUNT} PARENT_SCOPE)
    endif()
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
    string(REPLACE "|" ";" SHADER_PARTS "${SHADER_DEF}")
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

    # 发射顺序与旧版逐段一致：HLSL VS/PS/CS，(MS 则) MS+MeshPS，GLSL VS/PS/CS。
    if(HAS_HLSL)
        if(HAS_VS)
            compile_stage("${NAME}" HLSL VS)
        endif()
        if(HAS_PS)
            compile_stage("${NAME}" HLSL PS)
        endif()
        if(HAS_CS)
            compile_stage("${NAME}" HLSL CS)
        endif()
        if(HAS_MS)
            compile_stage("${NAME}" HLSL MS)
            compile_stage("${NAME}" HLSL MeshPS)
        endif()
    endif()

    if(HAS_GLSL)
        if(HAS_VS)
            compile_stage("${NAME}" GLSL VS)
        endif()
        if(HAS_PS)
            compile_stage("${NAME}" GLSL PS)
        endif()
        if(HAS_CS)
            compile_stage("${NAME}" GLSL CS)
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

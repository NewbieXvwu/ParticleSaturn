# 单源着色器翻译（D-004 试点，2026-07-26 拍板 DXC + SPIRV-Cross）。
# 工具是 macOS 构建前置（与 glsl410 的 glslangValidator REQUIRED 同例）：
# dxc 官方不发 macOS 二进制，用 scripts/build_dxc_macos.sh 构建安装。
find_program(PARTICLESATURN_DXC dxc HINTS "$ENV{HOME}/.local/opt/dxc/bin")
find_program(PARTICLESATURN_SPIRV_CROSS spirv-cross)
if(NOT PARTICLESATURN_DXC)
    message(FATAL_ERROR "dxc 未找到：运行 scripts/build_dxc_macos.sh（安装到 ~/.local/opt/dxc）或将 dxc 加入 PATH")
endif()
if(NOT PARTICLESATURN_SPIRV_CROSS)
    message(FATAL_ERROR "spirv-cross 未找到：brew install spirv-cross")
endif()

set(PARTICLESATURN_SINGLE_OUTPUT "${CMAKE_BINARY_DIR}/generated/single")
file(MAKE_DIRECTORY "${PARTICLESATURN_SINGLE_OUTPUT}")

set(PARTICLESATURN_SINGLE_ARTIFACTS)
foreach(SHADER_NAME ToneMap)
    set(SOURCE "${CMAKE_CURRENT_LIST_DIR}/${SHADER_NAME}.hlsl")
    set(SPV "${PARTICLESATURN_SINGLE_OUTPUT}/${SHADER_NAME}.spv")
    set(FRAG "${PARTICLESATURN_SINGLE_OUTPUT}/${SHADER_NAME}.gen.frag")
    set(MSL "${PARTICLESATURN_SINGLE_OUTPUT}/${SHADER_NAME}.gen.metal")
    add_custom_command(
        OUTPUT "${SPV}"
        COMMAND "${PARTICLESATURN_DXC}" -T ps_6_0 -E main -spirv -Fo "${SPV}" "${SOURCE}"
        DEPENDS "${SOURCE}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${FRAG}"
        COMMAND "${PARTICLESATURN_SPIRV_CROSS}" --version 410 --no-420pack-extension "${SPV}" --output "${FRAG}"
        DEPENDS "${SPV}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${MSL}"
        COMMAND "${PARTICLESATURN_SPIRV_CROSS}" --msl --msl-version 20000 "${SPV}" --output "${MSL}"
        DEPENDS "${SPV}"
        VERBATIM)
    list(APPEND PARTICLESATURN_SINGLE_ARTIFACTS "${SPV}" "${FRAG}" "${MSL}")
endforeach()

add_custom_target(ParticleSaturnSingleShaders ALL DEPENDS ${PARTICLESATURN_SINGLE_ARTIFACTS})

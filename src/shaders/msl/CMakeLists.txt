if(NOT PARTICLESATURN_SHADER_TOOLCHAIN)
    add_custom_target(ParticleSaturnMetalShaders)  # CI unit 层空桩（见顶层 option）
    return()
endif()
find_program(PARTICLESATURN_XCRUN xcrun REQUIRED)

set(PARTICLE_ABI_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/generated/shaders")
set(PARTICLESATURN_METAL_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/generated/metal")
set(PARTICLESATURN_METAL_LIBRARY "${PARTICLESATURN_METAL_OUTPUT_DIRECTORY}/ParticleKernels.metallib")

# metallib 输入：手写内核 + 全屏三角样板 + 单源翻译产物（D-004）。
set(PARTICLESATURN_METAL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ParticleKernels.metal"
    "${CMAKE_CURRENT_LIST_DIR}/FullscreenTriangle.metal"
    "${CMAKE_BINARY_DIR}/generated/single/ToneMap.gen.metal"
    "${CMAKE_BINARY_DIR}/generated/single/BloomDownsample.gen.metal"
    "${CMAKE_BINARY_DIR}/generated/single/KawaseBlur.gen.metal")

set(PARTICLESATURN_METAL_AIRS)
foreach(SOURCE IN LISTS PARTICLESATURN_METAL_SOURCES)
    get_filename_component(NAME "${SOURCE}" NAME_WE)
    set(AIR "${PARTICLESATURN_METAL_OUTPUT_DIRECTORY}/${NAME}.air")
    add_custom_command(
        OUTPUT "${AIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PARTICLESATURN_METAL_OUTPUT_DIRECTORY}"
        COMMAND "${PARTICLESATURN_XCRUN}" -sdk macosx metal -I "${PARTICLE_ABI_OUTPUT_DIRECTORY}" -c "${SOURCE}" -o "${AIR}"
        DEPENDS "${SOURCE}" "${PARTICLE_ABI_MSL}"
        VERBATIM
    )
    list(APPEND PARTICLESATURN_METAL_AIRS "${AIR}")
endforeach()

add_custom_command(
    OUTPUT "${PARTICLESATURN_METAL_LIBRARY}"
    COMMAND "${PARTICLESATURN_XCRUN}" -sdk macosx metallib ${PARTICLESATURN_METAL_AIRS} -o "${PARTICLESATURN_METAL_LIBRARY}"
    DEPENDS ${PARTICLESATURN_METAL_AIRS}
    VERBATIM
)

add_custom_target(ParticleSaturnMetalShaders ALL DEPENDS "${PARTICLESATURN_METAL_LIBRARY}")
add_dependencies(ParticleSaturnMetalShaders ParticleSaturnShaderAbi ParticleSaturnSingleShaders)

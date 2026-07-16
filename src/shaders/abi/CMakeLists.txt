set(PARTICLE_ABI_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/generated/shaders")
set(PARTICLE_ABI_HEADER "${PARTICLE_ABI_OUTPUT_DIRECTORY}/ParticleAbi.h")
set(PARTICLE_ABI_HLSL "${PARTICLE_ABI_OUTPUT_DIRECTORY}/Particle.hlsl")
set(PARTICLE_ABI_GLSL "${PARTICLE_ABI_OUTPUT_DIRECTORY}/Particle.glsl")

add_custom_command(
    OUTPUT "${PARTICLE_ABI_HEADER}" "${PARTICLE_ABI_HLSL}" "${PARTICLE_ABI_GLSL}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_LIST_DIR}/particle_abi.json"
        "-DOUTPUT_DIRECTORY=${PARTICLE_ABI_OUTPUT_DIRECTORY}" -P "${CMAKE_SOURCE_DIR}/cmake/GenerateShaderAbi.cmake"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/particle_abi.json" "${CMAKE_SOURCE_DIR}/cmake/GenerateShaderAbi.cmake"
    VERBATIM
)

add_custom_target(ParticleSaturnShaderAbi DEPENDS "${PARTICLE_ABI_HEADER}" "${PARTICLE_ABI_HLSL}" "${PARTICLE_ABI_GLSL}")

if(BUILD_TESTING)
    add_executable(ParticleSaturnShaderAbiTests "${CMAKE_SOURCE_DIR}/tests/ShaderAbiTests.cpp")
    add_dependencies(ParticleSaturnShaderAbiTests ParticleSaturnShaderAbi)
    target_include_directories(ParticleSaturnShaderAbiTests PRIVATE "${PARTICLE_ABI_OUTPUT_DIRECTORY}")
    target_compile_features(ParticleSaturnShaderAbiTests PRIVATE cxx_std_20)
    add_test(NAME ParticleSaturnShaderAbiTests COMMAND ParticleSaturnShaderAbiTests)
endif()

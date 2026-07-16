add_library(ParticleSaturnGpu STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../render/passes/ParticleSimulationStrategy.cpp"
)

target_include_directories(ParticleSaturnGpu
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/.."
)

target_compile_features(ParticleSaturnGpu PUBLIC cxx_std_20)
add_library(ParticleSaturn.GpuApi ALIAS ParticleSaturnGpu)

if(BUILD_TESTING)
    add_executable(ParticleSaturnGpuTests
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/GpuTests.cpp"
    )
    target_link_libraries(ParticleSaturnGpuTests PRIVATE ParticleSaturnGpu)
    add_test(NAME ParticleSaturnGpuTests COMMAND ParticleSaturnGpuTests)
endif()

# GPU 设备契约头（范围冻结，D-002）——纯头文件接口库。
add_library(ParticleSaturnGpu INTERFACE)

target_include_directories(ParticleSaturnGpu
    INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/.."
)

target_compile_features(ParticleSaturnGpu INTERFACE cxx_std_20)
add_library(ParticleSaturn.GpuApi ALIAS ParticleSaturnGpu)

if(BUILD_TESTING)
    add_executable(ParticleSaturnGpuTests
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/GpuTests.cpp"
    )
    target_link_libraries(ParticleSaturnGpuTests PRIVATE ParticleSaturnGpu)
    add_test(NAME ParticleSaturnGpuTests COMMAND ParticleSaturnGpuTests)
    set_tests_properties(ParticleSaturnGpuTests PROPERTIES LABELS unit)
endif()

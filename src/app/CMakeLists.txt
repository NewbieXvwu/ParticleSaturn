cmake_minimum_required(VERSION 3.19)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(ParticleSaturnAppCore LANGUAGES CXX)
    include(CTest)
endif()

add_library(ParticleSaturnAppCore STATIC
    AppController.cpp
    FrameCoordinator.cpp
)

target_include_directories(ParticleSaturnAppCore
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_features(ParticleSaturnAppCore PUBLIC cxx_std_20)
add_library(ParticleSaturn.Core ALIAS ParticleSaturnAppCore)

if(BUILD_TESTING)
    add_executable(ParticleSaturnAppCoreTests
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/AppCoreTests.cpp"
    )
    target_link_libraries(ParticleSaturnAppCoreTests PRIVATE ParticleSaturnAppCore)
    add_test(NAME ParticleSaturnAppCoreTests COMMAND ParticleSaturnAppCoreTests)
    set_tests_properties(ParticleSaturnAppCoreTests PROPERTIES LABELS unit)

    add_executable(ParticleSaturnObjectShaderToggleTests
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/ObjectShaderToggleTests.cpp"
    )
    target_link_libraries(ParticleSaturnObjectShaderToggleTests PRIVATE ParticleSaturnAppCore)
    add_test(NAME ParticleSaturnObjectShaderToggleTests COMMAND ParticleSaturnObjectShaderToggleTests)
    set_tests_properties(ParticleSaturnObjectShaderToggleTests PROPERTIES LABELS unit)
endif()

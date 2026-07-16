add_library(ParticleSaturnRender STATIC
    RenderGraph.cpp
    ResourceRegistry.cpp
)

target_include_directories(ParticleSaturnRender PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_features(ParticleSaturnRender PUBLIC cxx_std_20)
target_link_libraries(ParticleSaturnRender PUBLIC ParticleSaturnGpu)
add_library(ParticleSaturn.Render ALIAS ParticleSaturnRender)

if(BUILD_TESTING)
    add_executable(ParticleSaturnRenderTests "${CMAKE_CURRENT_LIST_DIR}/../../tests/RenderTests.cpp")
    target_link_libraries(ParticleSaturnRenderTests PRIVATE ParticleSaturnRender)
    add_test(NAME ParticleSaturnRenderTests COMMAND ParticleSaturnRenderTests)
endif()

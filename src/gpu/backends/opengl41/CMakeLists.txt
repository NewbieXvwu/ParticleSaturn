add_library(ParticleSaturnOpenGL41Backend STATIC OpenGL41Surface.mm OpenGLParticleSystem.mm OpenGLStarField.mm
    OpenGLRenderTargets.mm OpenGLBloom.mm OpenGLToneMapper.mm OpenGLSevenSegmentFps.mm OpenGLFrameRenderer.mm)
add_library(ParticleSaturn.Backend.OpenGL41 ALIAS ParticleSaturnOpenGL41Backend)
target_include_directories(ParticleSaturnOpenGL41Backend PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_include_directories(ParticleSaturnOpenGL41Backend PUBLIC "${CMAKE_BINARY_DIR}/generated/shaders")
target_compile_definitions(ParticleSaturnOpenGL41Backend PRIVATE GL_SILENCE_DEPRECATION=1)
target_link_libraries(ParticleSaturnOpenGL41Backend PUBLIC ParticleSaturnRender "-framework Cocoa" "-framework OpenGL")
add_dependencies(ParticleSaturnOpenGL41Backend ParticleSaturnShaderAbi)

if(BUILD_TESTING)
    add_executable(ParticleSaturnOpenGLParticleTests "${CMAKE_SOURCE_DIR}/tests/OpenGLParticleSystemTests.mm")
    target_compile_definitions(ParticleSaturnOpenGLParticleTests PRIVATE GL_SILENCE_DEPRECATION=1)
    target_link_libraries(ParticleSaturnOpenGLParticleTests PRIVATE ParticleSaturnOpenGL41Backend)
    add_dependencies(ParticleSaturnOpenGLParticleTests ParticleSaturnOpenGL41Shaders)
    add_test(NAME ParticleSaturnOpenGLParticleTests COMMAND ParticleSaturnOpenGLParticleTests "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleSimulationTF.vert" "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleRender.vert" "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleRender.frag" "${CMAKE_SOURCE_DIR}/src/shaders/glsl410")
    set_tests_properties(ParticleSaturnOpenGLParticleTests PROPERTIES LABELS gpu)
    add_executable(ParticleSaturnCrossBackendParticleTests "${CMAKE_SOURCE_DIR}/tests/CrossBackendParticleSystemTests.mm")
    target_compile_definitions(ParticleSaturnCrossBackendParticleTests PRIVATE GL_SILENCE_DEPRECATION=1)
    target_link_libraries(ParticleSaturnCrossBackendParticleTests PRIVATE ParticleSaturnMetalBackend ParticleSaturnOpenGL41Backend)
    add_dependencies(ParticleSaturnCrossBackendParticleTests ParticleSaturnMetalShaders ParticleSaturnOpenGL41Shaders)
    add_test(NAME ParticleSaturnCrossBackendParticleTests COMMAND ParticleSaturnCrossBackendParticleTests
        "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleSimulationTF.vert"
        "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleRender.vert"
        "${CMAKE_SOURCE_DIR}/src/shaders/glsl410/ParticleRender.frag")
    set_tests_properties(ParticleSaturnCrossBackendParticleTests PROPERTIES LABELS gpu)
endif()

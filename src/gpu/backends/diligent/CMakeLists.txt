add_library(ParticleSaturnDiligentVulkanAdapter STATIC DiligentVulkanAdapter.cpp)
add_library(ParticleSaturn.Backend.DiligentVulkan ALIAS ParticleSaturnDiligentVulkanAdapter)
target_include_directories(ParticleSaturnDiligentVulkanAdapter PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_include_directories(ParticleSaturnDiligentVulkanAdapter PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Platforms/Apple/interface")
target_link_libraries(ParticleSaturnDiligentVulkanAdapter
    PUBLIC ParticleSaturnVulkanRuntime Diligent-GraphicsEngineVk-static)

if(BUILD_TESTING)
    add_executable(ParticleSaturnDiligentVulkanAdapterTests
        "${CMAKE_SOURCE_DIR}/tests/DiligentVulkanAdapterTests.cpp")
    target_link_libraries(ParticleSaturnDiligentVulkanAdapterTests PRIVATE ParticleSaturnDiligentVulkanAdapter)
    if(TARGET ParticleSaturn.macOS)
        add_dependencies(ParticleSaturnDiligentVulkanAdapterTests ParticleSaturn.macOS)
    endif()
    add_test(NAME ParticleSaturnDiligentVulkanAdapterTests COMMAND ParticleSaturnDiligentVulkanAdapterTests
        "${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources")
endif()

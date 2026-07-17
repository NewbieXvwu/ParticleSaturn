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
    if(TARGET ParticleSaturnPlatformMacOS)
        add_executable(ParticleSaturnDiligentVulkanSurfaceTests
            "${CMAKE_SOURCE_DIR}/tests/DiligentVulkanSurfaceTests.mm")
        target_link_libraries(ParticleSaturnDiligentVulkanSurfaceTests PRIVATE
            ParticleSaturnDiligentVulkanAdapter ParticleSaturnPlatformMacOS)
        add_dependencies(ParticleSaturnDiligentVulkanSurfaceTests ParticleSaturn.macOS)
        add_test(NAME ParticleSaturnDiligentVulkanMoltenSurfaceTests COMMAND ParticleSaturnDiligentVulkanSurfaceTests
            "${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources" molten)
        if(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR)
            add_test(NAME ParticleSaturnDiligentVulkanKosmicSurfaceTests COMMAND ParticleSaturnDiligentVulkanSurfaceTests
                "${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources" kosmic)
            set_tests_properties(ParticleSaturnDiligentVulkanMoltenSurfaceTests ParticleSaturnDiligentVulkanKosmicSurfaceTests
                PROPERTIES TIMEOUT 30)
        else()
            set_tests_properties(ParticleSaturnDiligentVulkanMoltenSurfaceTests PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()

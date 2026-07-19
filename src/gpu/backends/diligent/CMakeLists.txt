add_library(ParticleSaturnDiligentVulkanAdapter STATIC
    DiligentVulkanAdapter.cpp
    "${CMAKE_SOURCE_DIR}/src/Diligent/DiligentShaderSources.cpp")
add_library(ParticleSaturn.Backend.DiligentVulkan ALIAS ParticleSaturnDiligentVulkanAdapter)
target_include_directories(ParticleSaturnDiligentVulkanAdapter PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_include_directories(ParticleSaturnDiligentVulkanAdapter PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Common/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Graphics/GraphicsEngine/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Platforms/Apple/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Primitives/interface")
target_link_libraries(ParticleSaturnDiligentVulkanAdapter
    PUBLIC ParticleSaturnRender ParticleSaturnVulkanRuntime ParticleSaturnMacOSImGui Diligent-GraphicsEngineVk-static)

if(TARGET ParticleSaturn.macOS)
    target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnDiligentVulkanAdapter)
endif()

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
            add_test(NAME ParticleSaturnVulkanKosmicPackageLayout
                COMMAND "${CMAKE_COMMAND}"
                    "-DRESOURCES=${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources"
                    -P "${CMAKE_SOURCE_DIR}/tests/VerifyKosmicKrispBundle.cmake")
            set_tests_properties(ParticleSaturnDiligentVulkanMoltenSurfaceTests ParticleSaturnDiligentVulkanKosmicSurfaceTests
                PROPERTIES TIMEOUT 30)
            set_tests_properties(ParticleSaturnVulkanKosmicPackageLayout PROPERTIES TIMEOUT 10)
        else()
            set_tests_properties(ParticleSaturnDiligentVulkanMoltenSurfaceTests PROPERTIES TIMEOUT 30)
        endif()
        add_test(NAME ParticleSaturnVulkanApplicationMoltenSmoke
            COMMAND "${CMAKE_COMMAND}" -E env
                PARTICLESATURN_GRAPHICS_API=vulkan
                PARTICLESATURN_VULKAN_DRIVER=molten
                PARTICLESATURN_VULKAN_SMOKE_FRAMES=3
                PARTICLESATURN_VULKAN_INTERACTION_SMOKE=1
                PARTICLESATURN_VULKAN_LOD_SMOKE=1
                "$<TARGET_FILE:ParticleSaturn.macOS>")
        set_tests_properties(ParticleSaturnVulkanApplicationMoltenSmoke PROPERTIES TIMEOUT 30)
        add_test(NAME ParticleSaturnVulkanApplicationMoltenRecovery
            COMMAND "${CMAKE_COMMAND}" -E env
                PARTICLESATURN_GRAPHICS_API=vulkan
                PARTICLESATURN_VULKAN_DRIVER=molten
                PARTICLESATURN_VULKAN_SMOKE_FRAMES=3
                PARTICLESATURN_VULKAN_DEVICE_LOST_SMOKE=1
                "$<TARGET_FILE:ParticleSaturn.macOS>")
        set_tests_properties(ParticleSaturnVulkanApplicationMoltenRecovery PROPERTIES TIMEOUT 30)
        add_test(NAME ParticleSaturnVulkanApplicationMoltenRestart
            COMMAND "${CMAKE_COMMAND}" -E env
                PARTICLESATURN_GRAPHICS_API=vulkan
                PARTICLESATURN_VULKAN_DRIVER=molten
                PARTICLESATURN_VULKAN_RESTART_SMOKE=1
                "$<TARGET_FILE:ParticleSaturn.macOS>")
        set_tests_properties(ParticleSaturnVulkanApplicationMoltenRestart PROPERTIES TIMEOUT 30)
        add_test(NAME ParticleSaturnVulkanApplicationMoltenPerformanceLock
            COMMAND "${CMAKE_COMMAND}" -E env
                PARTICLESATURN_GRAPHICS_API=vulkan
                PARTICLESATURN_VULKAN_DRIVER=molten
                PARTICLESATURN_PERFORMANCE_LOCK_SMOKE=3
                "$<TARGET_FILE:ParticleSaturn.macOS>")
        set_tests_properties(ParticleSaturnVulkanApplicationMoltenPerformanceLock PROPERTIES TIMEOUT 30)
        if(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR)
            add_test(NAME ParticleSaturnVulkanApplicationKosmicSmoke
                COMMAND "${CMAKE_COMMAND}" -E env
                    PARTICLESATURN_GRAPHICS_API=vulkan
                    PARTICLESATURN_VULKAN_DRIVER=kosmic
                    PARTICLESATURN_VULKAN_SMOKE_FRAMES=3
                    PARTICLESATURN_VULKAN_INTERACTION_SMOKE=1
                    PARTICLESATURN_VULKAN_LOD_SMOKE=1
                    "$<TARGET_FILE:ParticleSaturn.macOS>")
            set_tests_properties(ParticleSaturnVulkanApplicationKosmicSmoke PROPERTIES TIMEOUT 30)
            add_test(NAME ParticleSaturnVulkanApplicationKosmicRecovery
                COMMAND "${CMAKE_COMMAND}" -E env
                    PARTICLESATURN_GRAPHICS_API=vulkan
                    PARTICLESATURN_VULKAN_DRIVER=kosmic
                    PARTICLESATURN_VULKAN_SMOKE_FRAMES=3
                    PARTICLESATURN_VULKAN_DEVICE_LOST_SMOKE=1
                    "$<TARGET_FILE:ParticleSaturn.macOS>")
            set_tests_properties(ParticleSaturnVulkanApplicationKosmicRecovery PROPERTIES TIMEOUT 30)
            add_test(NAME ParticleSaturnVulkanApplicationKosmicRestart
                COMMAND "${CMAKE_COMMAND}" -E env
                    PARTICLESATURN_GRAPHICS_API=vulkan
                    PARTICLESATURN_VULKAN_DRIVER=kosmic
                    PARTICLESATURN_VULKAN_RESTART_SMOKE=1
                    "$<TARGET_FILE:ParticleSaturn.macOS>")
            set_tests_properties(ParticleSaturnVulkanApplicationKosmicRestart PROPERTIES TIMEOUT 30)
            add_test(NAME ParticleSaturnVulkanApplicationKosmicPerformanceLock
                COMMAND "${CMAKE_COMMAND}" -E env
                    PARTICLESATURN_GRAPHICS_API=vulkan
                    PARTICLESATURN_VULKAN_DRIVER=kosmic
                    PARTICLESATURN_PERFORMANCE_LOCK_SMOKE=3
                    "$<TARGET_FILE:ParticleSaturn.macOS>")
            set_tests_properties(ParticleSaturnVulkanApplicationKosmicPerformanceLock PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()

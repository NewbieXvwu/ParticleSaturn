add_library(ParticleSaturnVulkanRuntime STATIC VulkanDriverRuntime.cpp)
target_include_directories(ParticleSaturnVulkanRuntime PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnVulkanRuntime PUBLIC ParticleSaturnAppCore)

if(BUILD_TESTING)
    add_executable(ParticleSaturnVulkanRuntimeTests "${CMAKE_SOURCE_DIR}/tests/VulkanDriverRuntimeTests.cpp")
    target_link_libraries(ParticleSaturnVulkanRuntimeTests PRIVATE ParticleSaturnVulkanRuntime)
    add_test(NAME ParticleSaturnVulkanRuntimeTests COMMAND ParticleSaturnVulkanRuntimeTests
        "${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources")
    set_tests_properties(ParticleSaturnVulkanRuntimeTests PROPERTIES LABELS unit)
endif()

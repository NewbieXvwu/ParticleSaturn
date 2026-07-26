add_library(ParticleSaturnVulkanRuntime STATIC VulkanDriverRuntime.cpp)
target_include_directories(ParticleSaturnVulkanRuntime PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnVulkanRuntime PUBLIC ParticleSaturnAppCore)

if(BUILD_TESTING)
    add_executable(ParticleSaturnVulkanRuntimeTests "${CMAKE_SOURCE_DIR}/tests/VulkanDriverRuntimeTests.cpp")
    target_link_libraries(ParticleSaturnVulkanRuntimeTests PRIVATE ParticleSaturnVulkanRuntime)
    add_test(NAME ParticleSaturnVulkanRuntimeTests COMMAND ParticleSaturnVulkanRuntimeTests
        "${CMAKE_BINARY_DIR}/ParticleSaturn.macOS.app/Contents/Resources")
    # 依赖打包 app bundle 里的 Vulkan loader/ICD（brew molten-vk 装配产物），
    # 裸 CI runner 上无法通过——归 gpu 层（本机环境依赖）而非 unit。
    set_tests_properties(ParticleSaturnVulkanRuntimeTests PROPERTIES LABELS gpu)
endif()

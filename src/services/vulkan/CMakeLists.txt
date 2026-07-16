add_library(ParticleSaturnVulkanRuntime STATIC VulkanDriverRuntime.cpp)
target_include_directories(ParticleSaturnVulkanRuntime PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnVulkanRuntime PUBLIC ParticleSaturnAppCore)

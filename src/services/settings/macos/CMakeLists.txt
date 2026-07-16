add_library(ParticleSaturnMacOSSettings STATIC NSUserDefaultsStore.mm)
target_include_directories(ParticleSaturnMacOSSettings PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../../..")
target_link_libraries(ParticleSaturnMacOSSettings PUBLIC ParticleSaturnAppCore "-framework Foundation")

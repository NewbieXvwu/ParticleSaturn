add_library(ParticleSaturnMacOSResources STATIC BundleResources.mm)
target_include_directories(ParticleSaturnMacOSResources PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../../..")
target_link_libraries(ParticleSaturnMacOSResources PUBLIC "-framework Foundation")

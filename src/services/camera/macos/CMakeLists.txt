add_library(ParticleSaturnMacOSCamera STATIC AVFoundationCamera.mm)
target_include_directories(ParticleSaturnMacOSCamera PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_link_libraries(ParticleSaturnMacOSCamera PUBLIC "-framework AVFoundation" "-framework CoreVideo")

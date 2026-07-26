add_library(ParticleSaturnMacOSCamera STATIC AVFoundationCamera.mm CameraSelectorWindow.mm)
target_include_directories(ParticleSaturnMacOSCamera PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_link_libraries(ParticleSaturnMacOSCamera PUBLIC
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo")

if(BUILD_TESTING)
    add_executable(ParticleSaturnAVFoundationCameraErrorTests "${CMAKE_SOURCE_DIR}/tests/AVFoundationCameraErrorTests.mm")
    target_link_libraries(ParticleSaturnAVFoundationCameraErrorTests PRIVATE ParticleSaturnMacOSCamera "-framework Foundation")
    add_test(NAME ParticleSaturnAVFoundationCameraErrorTests COMMAND ParticleSaturnAVFoundationCameraErrorTests)
    set_tests_properties(ParticleSaturnAVFoundationCameraErrorTests PROPERTIES LABELS unit)
endif()

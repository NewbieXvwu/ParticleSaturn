set(PARTICLESATURN_TFLITE_ARM64_DIR "/tmp/particlesaturn-tflite-arm64" CACHE PATH
    "ARM64 TensorFlow Lite build directory produced by scripts/build_tflite_macos.sh")

add_library(ParticleSaturnMacOSHandTrackingWorker STATIC HandTrackingWorker.cpp)
target_include_directories(ParticleSaturnMacOSHandTrackingWorker PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_link_libraries(ParticleSaturnMacOSHandTrackingWorker PUBLIC ParticleSaturnAppCore)

if(BUILD_TESTING)
    add_executable(ParticleSaturnHandTrackingWorkerTests "${CMAKE_SOURCE_DIR}/tests/HandTrackingWorkerTests.cpp")
    target_link_libraries(ParticleSaturnHandTrackingWorkerTests PRIVATE ParticleSaturnMacOSHandTrackingWorker)
    add_test(NAME ParticleSaturnHandTrackingWorkerTests COMMAND ParticleSaturnHandTrackingWorkerTests)
    set_tests_properties(ParticleSaturnHandTrackingWorkerTests PROPERTIES LABELS unit)
endif()

set(PARTICLESATURN_TFLITE_LIBRARY "${PARTICLESATURN_TFLITE_ARM64_DIR}/libtensorflow-lite.a")
set(PARTICLESATURN_XNNPACK_DELEGATE_LIBRARY "${PARTICLESATURN_TFLITE_ARM64_DIR}/libxnnpack-delegate.a")
if(NOT EXISTS "${PARTICLESATURN_TFLITE_LIBRARY}" OR NOT EXISTS "${PARTICLESATURN_XNNPACK_DELEGATE_LIBRARY}")
    message(STATUS "Skipping macOS XNNPACK runtime; run scripts/build_tflite_macos.sh first")
    return()
endif()

file(GLOB_RECURSE PARTICLESATURN_TFLITE_ARCHIVES CONFIGURE_DEPENDS "${PARTICLESATURN_TFLITE_ARM64_DIR}/*.a")
list(SORT PARTICLESATURN_TFLITE_ARCHIVES)
add_library(ParticleSaturnMacOSHandTracking STATIC XnnpackRuntime.cpp)
target_include_directories(ParticleSaturnMacOSHandTracking PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../.."
    PRIVATE "${CMAKE_SOURCE_DIR}/HandTracker/libs/tensorflow")
target_link_libraries(ParticleSaturnMacOSHandTracking PUBLIC "-Wl,-all_load" ${PARTICLESATURN_TFLITE_ARCHIVES}
    "-framework Accelerate" "-framework CoreFoundation")
target_compile_definitions(ParticleSaturnMacOSHandTracking PUBLIC PARTICLESATURN_HAS_XNNPACK_RUNTIME=1)

if(BUILD_TESTING)
    add_executable(ParticleSaturnXnnpackRuntimeTests "${CMAKE_SOURCE_DIR}/tests/XnnpackRuntimeTests.cpp")
    target_link_libraries(ParticleSaturnXnnpackRuntimeTests PRIVATE ParticleSaturnMacOSHandTracking)
    target_compile_definitions(ParticleSaturnXnnpackRuntimeTests PRIVATE
        PARTICLESATURN_PALM_MODEL_PATH="${CMAKE_SOURCE_DIR}/HandTracker/models/palm_detection_full.tflite"
        PARTICLESATURN_LANDMARK_MODEL_PATH="${CMAKE_SOURCE_DIR}/HandTracker/models/hand_landmark_full.tflite")
    add_test(NAME ParticleSaturnXnnpackRuntimeTests COMMAND ParticleSaturnXnnpackRuntimeTests)
    set_tests_properties(ParticleSaturnXnnpackRuntimeTests PROPERTIES LABELS unit)

    add_executable(ParticleSaturnHandTrackingPipelineTests
        "${CMAKE_SOURCE_DIR}/tests/HandTrackingPipelineTests.cpp")
    target_link_libraries(ParticleSaturnHandTrackingPipelineTests PRIVATE
        ParticleSaturnMacOSHandTracking ParticleSaturnMacOSHandTrackingWorker)
    target_compile_definitions(ParticleSaturnHandTrackingPipelineTests PRIVATE
        PARTICLESATURN_PALM_MODEL_PATH="${CMAKE_SOURCE_DIR}/HandTracker/models/palm_detection_full.tflite"
        PARTICLESATURN_LANDMARK_MODEL_PATH="${CMAKE_SOURCE_DIR}/HandTracker/models/hand_landmark_full.tflite")
    add_test(NAME ParticleSaturnHandTrackingPipelineTests COMMAND ParticleSaturnHandTrackingPipelineTests)
    set_tests_properties(ParticleSaturnHandTrackingPipelineTests PROPERTIES LABELS unit)
endif()

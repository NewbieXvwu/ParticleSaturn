add_library(ParticleSaturnPlatformMacOS STATIC CocoaHost.mm MacOSApplication.mm MacOSBackendSelection.cpp)
add_library(ParticleSaturn.Platform.macOS ALIAS ParticleSaturnPlatformMacOS)
target_include_directories(ParticleSaturnPlatformMacOS PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnPlatformMacOS PUBLIC ParticleSaturnAppCore "-framework Cocoa" "-framework QuartzCore" "-framework Metal")

add_executable(ParticleSaturn.macOS MACOSX_BUNDLE
    LauncherMain.mm
    Main.mm
    OpenGL41Main.mm
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_osx.mm"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_metal.mm"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_opengl3.cpp")
set_target_properties(ParticleSaturn.macOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.particlesaturn.app"
)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnPlatformMacOS ParticleSaturnMetalBackend ParticleSaturnOpenGL41Backend ParticleSaturnMacOSSettings)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnVulkanRuntime)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSCamera)
if(TARGET ParticleSaturnMacOSHandTracking)
    target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSHandTracking)
    target_compile_definitions(ParticleSaturn.macOS PRIVATE PARTICLESATURN_HAS_XNNPACK_RUNTIME=1)
endif()
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSResources)
target_include_directories(ParticleSaturn.macOS PRIVATE "${CMAKE_SOURCE_DIR}/libs/imgui" "${CMAKE_SOURCE_DIR}/libs/imgui/backends")
target_link_libraries(ParticleSaturn.macOS PRIVATE "-framework GameController" "-framework QuartzCore")
target_compile_definitions(ParticleSaturn.macOS PRIVATE GL_SILENCE_DEPRECATION=1)
add_dependencies(ParticleSaturn.macOS ParticleSaturnMetalShaders ParticleSaturnOpenGL41Shaders)
add_custom_target(ParticleSaturnMacOSMetalResource ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/ParticleKernels.metallib"
    DEPENDS ParticleSaturnMetalShaders
    VERBATIM)
add_dependencies(ParticleSaturn.macOS ParticleSaturnMacOSMetalResource)
set(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR "" CACHE PATH "KosmicKrisp outputs directory containing the driver and ICD")
if(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR)
    set(PARTICLESATURN_KOSMICKRISP_DRIVER "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/libvulkan_kosmickrisp.dylib")
    set(PARTICLESATURN_KOSMICKRISP_ICD "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/kosmickrisp_mesa_icd.json")
    if(NOT EXISTS "${PARTICLESATURN_KOSMICKRISP_DRIVER}")
        message(FATAL_ERROR "PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR must contain libvulkan_kosmickrisp.dylib")
    endif()
    if(EXISTS "${PARTICLESATURN_KOSMICKRISP_ICD}")
        file(READ "${PARTICLESATURN_KOSMICKRISP_ICD}" PARTICLESATURN_KOSMICKRISP_ICD_CONTENT)
        string(REPLACE "./libvulkan_kosmickrisp.dylib" "../../../lib/libvulkan_kosmickrisp.dylib"
            PARTICLESATURN_KOSMICKRISP_ICD_CONTENT "${PARTICLESATURN_KOSMICKRISP_ICD_CONTENT}")
    else()
        set(PARTICLESATURN_KOSMICKRISP_ICD_CONTENT
            "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n    \"library_path\": \"../../../lib/libvulkan_kosmickrisp.dylib\",\n    \"api_version\": \"1.3.0\"\n  }\n}\n")
    endif()
    set(PARTICLESATURN_KOSMICKRISP_BUNDLE_ICD "${CMAKE_CURRENT_BINARY_DIR}/KosmicKrisp_icd.json")
    file(WRITE "${PARTICLESATURN_KOSMICKRISP_BUNDLE_ICD}" "${PARTICLESATURN_KOSMICKRISP_ICD_CONTENT}")
endif()

if(BUILD_TESTING)
    add_executable(ParticleSaturnMacOSBackendSelectionTests "${CMAKE_SOURCE_DIR}/tests/MacOSBackendSelectionTests.cpp")
    target_link_libraries(ParticleSaturnMacOSBackendSelectionTests PRIVATE ParticleSaturnPlatformMacOS)
    add_test(NAME ParticleSaturnMacOSBackendSelectionTests COMMAND ParticleSaturnMacOSBackendSelectionTests)
    add_executable(ParticleSaturnVisualBaselineMetricsTests "${CMAKE_SOURCE_DIR}/tests/VisualBaselineMetricsTests.cpp")
    add_dependencies(ParticleSaturnVisualBaselineMetricsTests ParticleSaturn.macOS)
    add_test(NAME ParticleSaturnVisualBaselineTests
        COMMAND /bin/sh "${CMAKE_SOURCE_DIR}/tests/RunMacOSVisualBaseline.sh"
            "$<TARGET_FILE:ParticleSaturn.macOS>"
            "$<TARGET_FILE:ParticleSaturn.macOS>"
            "$<TARGET_FILE:ParticleSaturnVisualBaselineMetricsTests>"
            "${CMAKE_CURRENT_BINARY_DIR}/visual-baseline")
    set_tests_properties(ParticleSaturnVisualBaselineTests PROPERTIES TIMEOUT 30)
endif()
add_custom_command(TARGET ParticleSaturn.macOS POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/ParticleKernels.metallib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/glsl410"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/src/shaders/glsl410"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/glsl410"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/HandTracker/models/palm_detection_full.tflite"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/palm_detection_full.tflite"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/HandTracker/models/hand_landmark_full.tflite"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/hand_landmark_full.tflite"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/lib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/etc/vulkan/icd.d"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/lib/libvulkan.1.dylib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/lib/libMoltenVK.dylib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/etc/vulkan/icd.d/MoltenVK_icd.json"
)
if(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR)
    add_custom_command(TARGET ParticleSaturn.macOS POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PARTICLESATURN_KOSMICKRISP_DRIVER}"
            "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/lib/libvulkan_kosmickrisp.dylib"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PARTICLESATURN_KOSMICKRISP_BUNDLE_ICD}"
            "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/Vulkan/etc/vulkan/icd.d/KosmicKrisp_icd.json"
        VERBATIM)
endif()

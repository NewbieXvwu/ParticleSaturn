add_library(ParticleSaturnPlatformMacOS STATIC CocoaHost.mm MacOSApplication.mm MacOSBackendSelection.cpp
    SmokeHarness.cpp
    "${CMAKE_SOURCE_DIR}/src/services/diagnostics/macos/MacOSCrashHandler.mm")
add_library(ParticleSaturn.Platform.macOS ALIAS ParticleSaturnPlatformMacOS)
target_include_directories(ParticleSaturnPlatformMacOS PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnPlatformMacOS PUBLIC ParticleSaturnAppCore "-framework Cocoa" "-framework QuartzCore" "-framework Metal")

add_library(ParticleSaturnMacOSImGui STATIC
    "${CMAKE_SOURCE_DIR}/src/Diligent/ImGuiDiligent.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_osx.mm"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_metal.mm"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_opengl3.cpp")
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/Diligent/ImGuiDiligent.cpp"
    PROPERTIES LANGUAGE OBJCXX)
target_include_directories(ParticleSaturnMacOSImGui PUBLIC
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Common/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Graphics/GraphicsEngine/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Platforms/Apple/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Primitives/interface"
    "${CMAKE_SOURCE_DIR}/libs/imgui"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends")
target_compile_definitions(ParticleSaturnMacOSImGui PUBLIC PLATFORM_MACOS=1)
target_link_libraries(ParticleSaturnMacOSImGui PUBLIC
    "-framework Cocoa"
    "-framework GameController"
    "-framework Metal"
    "-framework QuartzCore")

add_executable(ParticleSaturn.macOS MACOSX_BUNDLE
    AppShell.mm
    LauncherMain.mm
    Main.mm
    OpenGL41Main.mm
    VulkanMain.mm
    MacOSMd3Panel.cpp)
set_target_properties(ParticleSaturn.macOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.particlesaturn.app"
)
target_link_libraries(ParticleSaturn.macOS PRIVATE
    ParticleSaturnPlatformMacOS
    ParticleSaturnMetalBackend
    ParticleSaturnOpenGL41Backend
    ParticleSaturnMacOSSettings
    ParticleSaturnMD3)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSCamera ParticleSaturnMacOSHandTrackingWorker)
if(TARGET ParticleSaturnMacOSHandTracking)
    target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSHandTracking)
    target_compile_definitions(ParticleSaturn.macOS PRIVATE PARTICLESATURN_HAS_XNNPACK_RUNTIME=1)
endif()
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSResources)
target_include_directories(ParticleSaturn.macOS PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Common/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Graphics/GraphicsEngine/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Platforms/Apple/interface"
    "${CMAKE_SOURCE_DIR}/libs/DiligentCore/Primitives/interface"
    "${CMAKE_SOURCE_DIR}/libs/imgui"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends")
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
if(PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR)
    set(PARTICLESATURN_KOSMICKRISP_DRIVER "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/libvulkan_kosmickrisp.dylib")
    set(PARTICLESATURN_KOSMICKRISP_ICD "")
    foreach(_kosmic_icd_candidate
        "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/kosmickrisp_mesa_icd.json"
        "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/KosmicKrisp_icd.json"
        "${PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR}/../share/vulkan/icd.d/KosmicKrisp_icd.json")
        if(EXISTS "${_kosmic_icd_candidate}")
            set(PARTICLESATURN_KOSMICKRISP_ICD "${_kosmic_icd_candidate}")
            break()
        endif()
    endforeach()
    if(NOT EXISTS "${PARTICLESATURN_KOSMICKRISP_DRIVER}")
        message(FATAL_ERROR "PARTICLESATURN_KOSMICKRISP_OUTPUT_DIR must contain libvulkan_kosmickrisp.dylib")
    endif()
    if(EXISTS "${PARTICLESATURN_KOSMICKRISP_ICD}")
        file(READ "${PARTICLESATURN_KOSMICKRISP_ICD}" PARTICLESATURN_KOSMICKRISP_ICD_CONTENT)
        string(REGEX REPLACE "\"library_path\"[ \t]*:[ \t]*\"[^\"]*\""
            "\"library_path\": \"../../../lib/libvulkan_kosmickrisp.dylib\""
            PARTICLESATURN_KOSMICKRISP_ICD_CONTENT "${PARTICLESATURN_KOSMICKRISP_ICD_CONTENT}")
    else()
        set(PARTICLESATURN_KOSMICKRISP_ICD_CONTENT
            "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n    \"library_path\": \"../../../lib/libvulkan_kosmickrisp.dylib\",\n    \"api_version\": \"1.3.0\"\n  }\n}\n")
    endif()
    set(PARTICLESATURN_KOSMICKRISP_BUNDLE_ICD "${CMAKE_CURRENT_BINARY_DIR}/KosmicKrisp_icd.json")
    file(WRITE "${PARTICLESATURN_KOSMICKRISP_BUNDLE_ICD}" "${PARTICLESATURN_KOSMICKRISP_ICD_CONTENT}")
endif()

if(BUILD_TESTING)
    add_executable(ParticleSaturnMacOSCrashHandlerTests "${CMAKE_SOURCE_DIR}/tests/MacOSCrashHandlerTests.mm")
    target_link_libraries(ParticleSaturnMacOSCrashHandlerTests PRIVATE ParticleSaturnPlatformMacOS)
    add_test(NAME ParticleSaturnMacOSCrashHandlerTests COMMAND ParticleSaturnMacOSCrashHandlerTests)
    set_tests_properties(ParticleSaturnMacOSCrashHandlerTests PROPERTIES LABELS unit)
    add_executable(ParticleSaturnMacOSBackendSelectionTests "${CMAKE_SOURCE_DIR}/tests/MacOSBackendSelectionTests.cpp")
    target_link_libraries(ParticleSaturnMacOSBackendSelectionTests PRIVATE ParticleSaturnPlatformMacOS)
    add_test(NAME ParticleSaturnMacOSBackendSelectionTests COMMAND ParticleSaturnMacOSBackendSelectionTests)
    set_tests_properties(ParticleSaturnMacOSBackendSelectionTests PROPERTIES LABELS unit)
    add_executable(ParticleSaturnVisualBaselineMetricsTests "${CMAKE_SOURCE_DIR}/tests/VisualBaselineMetricsTests.cpp")
    add_dependencies(ParticleSaturnVisualBaselineMetricsTests ParticleSaturn.macOS)
    add_test(NAME ParticleSaturnVisualBaselineTests
        COMMAND /bin/sh "${CMAKE_SOURCE_DIR}/tests/RunMacOSVisualBaseline.sh"
            "$<TARGET_FILE:ParticleSaturn.macOS>"
            "$<TARGET_FILE:ParticleSaturn.macOS>"
            "$<TARGET_FILE:ParticleSaturnVisualBaselineMetricsTests>"
            "${CMAKE_CURRENT_BINARY_DIR}/visual-baseline")
    set_tests_properties(ParticleSaturnVisualBaselineTests PROPERTIES TIMEOUT 30 LABELS app RUN_SERIAL TRUE)
    add_test(NAME ParticleSaturnMacOSPerformanceLockMetal
        COMMAND "${CMAKE_COMMAND}" -E env
            PARTICLESATURN_GRAPHICS_API=metal
            PARTICLESATURN_PERFORMANCE_LOCK_SMOKE=3
            "$<TARGET_FILE:ParticleSaturn.macOS>")
    add_test(NAME ParticleSaturnMacOSPerformanceLockOpenGL
        COMMAND "${CMAKE_COMMAND}" -E env
            PARTICLESATURN_GRAPHICS_API=opengl41
            PARTICLESATURN_PERFORMANCE_LOCK_SMOKE=3
            "$<TARGET_FILE:ParticleSaturn.macOS>")
    set_tests_properties(ParticleSaturnMacOSPerformanceLockMetal ParticleSaturnMacOSPerformanceLockOpenGL
        PROPERTIES TIMEOUT 30 LABELS app RUN_SERIAL TRUE)
    add_test(NAME ParticleSaturnMacOSFullscreenRestoreMetal
        COMMAND "${CMAKE_COMMAND}" -E env
            PARTICLESATURN_GRAPHICS_API=metal
            PARTICLESATURN_FULLSCREEN_RESTORE_SMOKE=3
            "$<TARGET_FILE:ParticleSaturn.macOS>")
    add_test(NAME ParticleSaturnMacOSFullscreenRestoreOpenGL
        COMMAND "${CMAKE_COMMAND}" -E env
            PARTICLESATURN_GRAPHICS_API=opengl41
            PARTICLESATURN_FULLSCREEN_RESTORE_SMOKE=3
            "$<TARGET_FILE:ParticleSaturn.macOS>")
    set_tests_properties(ParticleSaturnMacOSFullscreenRestoreMetal ParticleSaturnMacOSFullscreenRestoreOpenGL
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS app)
    # 退出码之外的第二道保险：即使将来再出现退出码被吞的回归，
    # 失败日志本身也会让 ctest 判负（D-008）。
    set_tests_properties(
        ParticleSaturnMacOSPerformanceLockMetal ParticleSaturnMacOSPerformanceLockOpenGL
        ParticleSaturnMacOSFullscreenRestoreMetal ParticleSaturnMacOSFullscreenRestoreOpenGL
        PROPERTIES FAIL_REGULAR_EXPRESSION "\\[smoke\\] FAILED")
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
        COMMAND "${CMAKE_COMMAND}"
            "-DRESOURCES=$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
            -P "${CMAKE_SOURCE_DIR}/tests/VerifyKosmicKrispBundle.cmake"
        VERBATIM)
endif()

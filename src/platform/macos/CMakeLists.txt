add_library(ParticleSaturnPlatformMacOS STATIC CocoaHost.mm)
add_library(ParticleSaturn.Platform.macOS ALIAS ParticleSaturnPlatformMacOS)
target_include_directories(ParticleSaturnPlatformMacOS PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnPlatformMacOS PUBLIC ParticleSaturnAppCore "-framework Cocoa" "-framework QuartzCore")

add_executable(ParticleSaturn.macOS MACOSX_BUNDLE
    Main.mm
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_osx.mm"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_metal.mm")
set_target_properties(ParticleSaturn.macOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.particlesaturn.app"
)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnPlatformMacOS ParticleSaturnMetalBackend)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnVulkanRuntime)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnMacOSResources)
target_include_directories(ParticleSaturn.macOS PRIVATE "${CMAKE_SOURCE_DIR}/libs/imgui" "${CMAKE_SOURCE_DIR}/libs/imgui/backends")
target_link_libraries(ParticleSaturn.macOS PRIVATE "-framework GameController")
add_dependencies(ParticleSaturn.macOS ParticleSaturnMetalShaders)
add_custom_target(ParticleSaturnMacOSMetalResource ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/ParticleKernels.metallib"
    DEPENDS ParticleSaturnMetalShaders
    VERBATIM)
add_dependencies(ParticleSaturn.macOS ParticleSaturnMacOSMetalResource)
add_custom_command(TARGET ParticleSaturn.macOS POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/ParticleKernels.metallib"
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

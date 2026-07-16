add_library(ParticleSaturnPlatformMacOS STATIC CocoaHost.mm)
add_library(ParticleSaturn.Platform.macOS ALIAS ParticleSaturnPlatformMacOS)
target_include_directories(ParticleSaturnPlatformMacOS PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(ParticleSaturnPlatformMacOS PUBLIC "-framework Cocoa" "-framework QuartzCore")

add_executable(ParticleSaturn.macOS MACOSX_BUNDLE Main.mm)
set_target_properties(ParticleSaturn.macOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.particlesaturn.app"
)
target_link_libraries(ParticleSaturn.macOS PRIVATE ParticleSaturnPlatformMacOS ParticleSaturnMetalBackend)
add_dependencies(ParticleSaturn.macOS ParticleSaturnMetalShaders)
add_custom_command(TARGET ParticleSaturn.macOS POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_BINARY_DIR}/generated/metal/ParticleKernels.metallib"
        "$<TARGET_FILE_DIR:ParticleSaturn.macOS>/../Resources/ParticleKernels.metallib"
)

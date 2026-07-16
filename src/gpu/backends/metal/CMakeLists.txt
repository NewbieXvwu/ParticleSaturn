add_library(ParticleSaturnMetalBackend STATIC MetalBackend.mm)
add_library(ParticleSaturn.Backend.Metal ALIAS ParticleSaturnMetalBackend)
target_include_directories(ParticleSaturnMetalBackend PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_link_libraries(ParticleSaturnMetalBackend PUBLIC "-framework Metal" "-framework QuartzCore")

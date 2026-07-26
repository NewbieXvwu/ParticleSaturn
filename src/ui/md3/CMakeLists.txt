# MD3 统一 UI 系统
# 支持 OpenGL（macOS/Windows）、Metal、Vulkan 后端

add_library(ParticleSaturnMD3 STATIC
    MD3Context.cpp
    MD3Widgets.cpp
    MD3Theme.cpp)

target_include_directories(ParticleSaturnMD3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/libs/imgui"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends")

# macOS/Windows 平台差异
if(APPLE)
    target_compile_definitions(ParticleSaturnMD3 PUBLIC MD3_HAS_OPENGL=1)
    target_compile_options(ParticleSaturnMD3 PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(ParticleSaturnMD3 PUBLIC "-framework OpenGL")
elseif(WIN32)
    target_compile_definitions(ParticleSaturnMD3 PUBLIC MD3_HAS_OPENGL=1)
    # Windows 使用 GLAD
    target_link_libraries(ParticleSaturnMD3 PUBLIC glad)
endif()

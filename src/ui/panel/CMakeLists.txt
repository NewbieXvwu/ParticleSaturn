# D-015：跨平台共享的 MD3 调试/设置面板。
# 平台中立（imgui + MD3 + AppController + DiagnosticBus），macOS 与 Windows 外壳共用。

add_library(ParticleSaturnMd3Panel STATIC
    Md3Panel.cpp)

target_compile_features(ParticleSaturnMd3Panel PUBLIC cxx_std_20)

# MD3 提供 MD3.h/MD3Log.h 及 imgui 头（PUBLIC）；AppCore 以 src 为根提供
# app/、services/、ui/ 头路径。
target_link_libraries(ParticleSaturnMd3Panel PUBLIC
    ParticleSaturnMD3
    ParticleSaturnAppCore)

add_library(ParticleSaturn.Md3Panel ALIAS ParticleSaturnMd3Panel)

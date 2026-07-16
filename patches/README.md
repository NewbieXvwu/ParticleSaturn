# 第三方补丁

此目录存放项目维护的第三方源码补丁。`scripts/apply_third_party_patch.cmake` 是唯一的应用入口，会先检查补丁是否已存在，再决定跳过或应用，因此可重复调用。Shell 与 Windows 批处理脚本仅转发到该入口。

| 补丁 | 目标 | 调用方 |
|---|---|---|
| `diligent-volk-loader-path.patch` | DiligentCore 的 volk | macOS CMake 配置 |
| `tflite-prune.patch`、`tflite-elementwise-compat.patch` | TensorFlow Lite | macOS 与 Windows TensorFlow Lite 构建脚本 |
| `imgui-md3.patch` | Dear ImGui | macOS 与 Windows 主 CMake 配置 |

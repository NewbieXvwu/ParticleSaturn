# 第三方补丁

此目录存放项目维护的第三方源码补丁。`scripts/apply_third_party_patch.sh` 会先检查补丁是否已存在，再决定跳过或应用，因此可重复调用。

| 补丁 | 目标 | 调用方 |
|---|---|---|
| `diligent-volk-loader-path.patch` | DiligentCore 的 volk | macOS CMake 配置 |
| `tflite-prune.patch` | TensorFlow Lite | `scripts/build_tflite_macos.sh` |
| `imgui-md3.patch` | Dear ImGui | 手工执行 `scripts/apply_third_party_patch.sh imgui-md3` |

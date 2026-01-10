# Diligent Engine 迁移（进行中）

本文件描述 `ParticleSaturn` 迁移到 Diligent Engine 的当前进度与构建方式。

当前阶段目标：让 **D3D12** 和 **Vulkan** 后端都能跑通（创建窗口、初始化设备/交换链、每帧清屏并 Present）。

## 代码位置

- CMake 入口：`CMakeLists.txt`
- 最小运行程序：`src_diligent/main.cpp`
- Diligent 初始化与渲染封装：`src_diligent/DiligentBackend.*`
- 超分预留接口（未实现）：`src_diligent/SuperResolution.h`

## 构建（CMake）

推荐使用 Visual Studio 的 CMake 集成或命令行：

```bash
cmake -S . -B build_diligent -A x64
cmake --build build_diligent --config Release
```

## 运行

```bash
# 默认：D3D12
build_diligent\\Release\\ParticleSaturn.Diligent.exe --backend=d3d12

# Vulkan
build_diligent\\Release\\ParticleSaturn.Diligent.exe --backend=vulkan
```

## 说明

- 目前只验证 Diligent 渲染后端链路，不包含粒子/Compute/MD3/ImGui 迁移。
- Vulkan 后端依赖显卡驱动提供 Vulkan 运行时（一般安装显卡驱动即可）。


# Diligent Engine 迁移（进行中）

本文件描述 `ParticleSaturn` 迁移到 Diligent Engine 的当前进度与构建方式。

当前阶段目标：让 **D3D12** 和 **Vulkan** 后端都能跑通（创建窗口、初始化设备/交换链、每帧清屏并 Present）。

## 代码位置

- CMake 入口：`CMakeLists.txt`
- 最小运行程序：`src/Diligent/Main.cpp`
- Diligent 初始化与渲染封装：`src/Diligent/DiligentBackend.*`
- 超分预留接口（未实现）：`src/Diligent/SuperResolution.h`

## 依赖/环境

- Vulkan 后端运行时：显卡驱动自带的 Vulkan Runtime（系统可加载 `vulkan-1.dll`）
- Vulkan SDK（推荐）：用于让 CMake 正确找到 Vulkan（`VULKAN_SDK` 环境变量）及提供工具链/验证层
- ATL（推荐）：VS Installer 安装 **C++ ATL** 组件
  - 若未安装 ATL，本仓库会自动启用 `shims/atl/*` 作为最小兼容（仅覆盖 DiligentCore 目前用到的部分）
  - 可用 `-DPARTICLESATURN_FORCE_ATL_SHIM=ON` 强制使用 shim（一般仅用于 CI/最小 VS 安装）

## 构建

### Visual Studio（解决方案）

- 项目：`ParticleSaturn.Diligent`
- 默认构建目录：`build_diligent\<Platform>\`
- 可执行文件：`build_diligent\<Platform>\ParticleSaturn.Diligent.exe`

### 命令行（CMake + NMake）

```bash
cmake -S . -B build_diligent_cli -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build_diligent_cli --target ParticleSaturn.Diligent
```

## 运行

```bash
# 默认：D3D12
build_diligent\\x64\\ParticleSaturn.Diligent.exe --backend=d3d12

# Vulkan
build_diligent\\x64\\ParticleSaturn.Diligent.exe --backend=vulkan
```

## 说明

- 目前只验证 Diligent 渲染后端链路，不包含粒子/Compute/MD3/ImGui 迁移。
- Vulkan 后端依赖显卡驱动提供 Vulkan 运行时（一般安装显卡驱动即可）；安装 Vulkan SDK 可让 CMake 检测与调试体验更好。

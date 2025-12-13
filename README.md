<p align="center">
  <img src="https://img.shields.io/badge/OpenGL-4.3-5586A4?style=for-the-badge&logo=opengl&logoColor=white" alt="OpenGL 4.3"/>
  <img src="https://img.shields.io/badge/Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white" alt="Windows"/>
  <img src="https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++"/>
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" alt="MIT License"/>
</p>

# 🪐 Particle Saturn

实时渲染的 OpenGL 土星模拟，支持手势追踪交互。

## ✨ 特性

- 🚀 GPU Compute Shader 驱动的粒子物理模拟
- 📊 动态 LOD：根据帧率自动调整粒子数量和渲染分辨率
- 🖐️ 手势追踪：通过摄像头捕捉手部动作控制土星旋转和缩放
- 🎨 Windows 11 Mica/Acrylic 背景模糊效果
- 🛠️ ImGui 调试面板（F3 切换）

## ⌨️ 快捷键

| 按键 | 功能 |
|:----:|:-----|
| `F3` | 显示/隐藏调试面板 |
| `F11` | 全屏切换 |
| `B` | 切换窗口背景效果 |
| `ESC` | 退出 |

## 🔧 构建

### 依赖

| 依赖 | 说明 |
|:-----|:-----|
| [Visual Studio 2026](https://visualstudio.microsoft.com/) | C++ 开发环境 |
| [vcpkg](https://github.com/microsoft/vcpkg) | C++ 包管理器 |
| [MediaPipe](https://github.com/google-ai-edge/mediapipe) | 手势追踪模型 |
| [TensorFlow Lite](https://www.tensorflow.org/lite) | 轻量级推理引擎 |
| [OpenCV](https://opencv.org/) | 计算机视觉库 |
| [Dear ImGui](https://github.com/ocornut/imgui) | 即时模式 GUI |
| [GLFW](https://www.glfw.org/) | 窗口管理 |
| [GLAD](https://glad.dav1d.de/) | OpenGL 加载器 |
| [GLM](https://github.com/g-truc/glm) | 数学库 |

### 步骤

```bash
# 1. 安装 vcpkg（如果还没有）
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && bootstrap-vcpkg.bat && vcpkg integrate install
cd ..

# 2. 克隆项目
git clone --recursive https://github.com/NewbieXvwu/ParticleSaturn.git
cd ParticleSaturn

# 3. 构建 OpenCV（自动配置、编译、安装，约 10-20 分钟）
scripts\build_opencv.cmd

# 4. 应用 TFLite 剪枝补丁并构建（自动配置、编译、安装，约 20-40 分钟）
git -C HandTracker/libs/tensorflow apply ../../scripts/tflite-prune.patch
scripts\build_tflite.cmd

# 5. 编译项目（首次编译时 vcpkg 会自动安装 GLFW、GLAD、GLM）
msbuild ParticleSaturn.slnx /p:Configuration=Release /p:Platform=x64
```

### Visual Studio 2022 用户

项目默认使用 VS2026 (v145) 工具集。如需使用 VS2022 编译：

1. 在项目属性中将 Platform Toolset 改为 `v143`
2. 编译时指定 OpenCV 运行时版本（VS2022 编译的 OpenCV 使用 `vc17` 目录）：
   ```bash
   msbuild ParticleSaturn.slnx /p:PlatformToolset=v143 /p:OpenCVRuntime=vc17 /p:Configuration=Release /p:Platform=x64
   ```

## 📄 许可

本项目基于 [MIT License](LICENSE) 开源。

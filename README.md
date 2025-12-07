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
| [MediaPipe](https://github.com/google-ai-edge/mediapipe) | 手势追踪 |
| [OpenCV](https://opencv.org/) | 计算机视觉库 |
| [Dear ImGui](https://github.com/ocornut/imgui) | 即时模式 GUI |
| [GLFW](https://www.glfw.org/) | 窗口管理 |
| [GLAD](https://glad.dav1d.de/) | OpenGL 加载器 |
| [GLM](https://github.com/g-truc/glm) | 数学库 |

### 步骤

```bash
git clone --recursive https://github.com/NewbieXvwu/ParticleSaturn.git
cd ParticleSaturn

# 构建 OpenCV（自动配置、编译、安装）
scripts\build_opencv.cmd

# 用 Visual Studio 打开 ParticleSaturn.slnx 编译项目
```

## 📄 许可

本项目基于 [MIT License](LICENSE) 开源。

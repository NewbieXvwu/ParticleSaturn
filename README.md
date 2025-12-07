# Particle Saturn

120 万粒子实时渲染的土星模拟器，支持手势追踪交互。

![OpenGL 4.3](https://img.shields.io/badge/OpenGL-4.3-blue)
![Windows](https://img.shields.io/badge/Platform-Windows-lightgrey)

## 特性

- GPU Compute Shader 驱动的粒子物理模拟
- 动态 LOD：根据帧率自动调整粒子数量和渲染分辨率
- 手势追踪：通过摄像头捕捉手部动作控制土星旋转和缩放
- Windows 11 Mica/Acrylic 背景模糊效果
- ImGui 调试面板（F3 切换）

## 快捷键

| 按键 | 功能 |
|------|------|
| F3 | 显示/隐藏调试面板 |
| F11 | 全屏切换 |
| B | 切换窗口背景效果 |
| ESC | 退出 |

## 构建

### 依赖

- Visual Studio 2022
- CMake

### 步骤

1. 克隆仓库
```bash
git clone https://github.com/NewbieXvwu/ParticleSaturn.git
cd ParticleSaturn
```

2. 下载并构建 OpenCV
```bash
git clone --depth 1 --branch 4.10.0 https://github.com/opencv/opencv.git HandTracker/libs/opencv
scripts\build_opencv.cmd
cmake --build HandTracker\libs\opencv\build --config Release
```

3. 用 Visual Studio 打开 `ParticleSaturn.slnx` 编译

## 许可

MIT

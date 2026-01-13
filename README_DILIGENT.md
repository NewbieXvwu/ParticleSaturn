# Diligent Engine 迁移（进行中）

本文件描述 `ParticleSaturn` 迁移到 Diligent Engine 的当前进度与构建方式。

当前阶段目标：推进到路线图 **阶段 5（ImGui 集成）**，实现调试 UI 和 FPS 显示。

## 代码位置

- CMake 入口：`CMakeLists.txt`
- 最小运行程序：`src/Diligent/Main.cpp`
- Diligent 初始化与渲染封装：`src/Diligent/DiligentBackend.*`
- ImGui 集成：`src/Diligent/ImGuiDiligent.*`（自定义 Diligent 渲染器）
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
- 推荐打开：`ParticleSaturn.sln`（传统 `.sln`）
- 默认构建目录：`bin_diligent\<Platform>\`
- 可执行文件：`bin_diligent\<Platform>\ParticleSaturn.Diligent.exe`

### 命令行（CMake + NMake）

```bash
cmake -S . -B bin_diligent_cli -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build bin_diligent_cli --target ParticleSaturn.Diligent
```

### 编译加速（推荐）

- VS 一键编译也能用 Ninja：`ParticleSaturn.Diligent` 是 Makefile 工程，VS 的“生成/重新生成”会调用 `scripts/build_diligent.ps1`，脚本会自动探测并优先使用 Ninja（找不到就回退 NMake）。
- 快速迭代配置：新增 `FastRelease`（只在该配置开启禁用 `/GL` 的快速构建；普通 `Release` 默认不启用）
  - VS：选择 `FastRelease|x64`（或 `FastRelease|Win32`）
  - 命令行（可选）：也可以用环境变量 `PARTICLESATURN_FAST_BUILD=1` 强制开启

## 运行

```bash
# 默认：D3D12
bin_diligent\\x64\\ParticleSaturn.Diligent.exe --backend=d3d12

# Vulkan
bin_diligent\\x64\\ParticleSaturn.Diligent.exe --backend=vulkan
```

## 说明

- 已完成（阶段 1 最小验证）：
  - SwapChain 创建深度缓冲（`D32_FLOAT`）
  - 基础 Graphics PSO（无顶点缓冲，`SV_VertexID/gl_VertexIndex` 生成全屏四边形）
  - 离屏 Render Target Texture（作为 FBO 等效）
  - 离屏渲染后通过全屏四边形合成到 SwapChain BackBuffer 并 Present（后续用于复刻 OpenGL 的 tone mapping 链路）
- 已推进（阶段 2 复刻版验证）：
  - 星空使用 3D 球壳分布（与 OpenGL 旧实现一致的随机分布方式）
  - 星空密度与分辨率无关：以 OpenGL 版在 1920x1080 下的星空密度（5 万）为基准，按像素面积缩放星星数量
  - 使用 view/proj/model 变换与深度相关的大小衰减（`aSize*(1000/-p.z)`）
  - 以“实例化 billboard 四边形”实现圆形点精灵（UV 圆形裁剪 + 软边 alpha），并用加法混合叠加
  - 每帧更新时间常量缓冲，星星闪烁逻辑沿用旧版噪声/正弦调制（用于验证 uniform/CB 链路）
- 已推进（阶段 3 第 1 步：粒子数据通路验证）：
  - CPU 侧复刻 `ComputeInitSaturn` 的初始化逻辑（生成本体/光环粒子分布与 RGBA8 打包颜色）
  - 粒子规模对齐 OpenGL 旧版默认值：120 万（对“遮蔽感/不透光感”影响很大；密度补偿只能补亮度不能补覆盖）
  - 创建 structured buffer 三缓冲（后续 compute 轮转用），当前先固定渲染缓冲
  - 复刻 `VertexSaturn/FragmentSaturn` 的点精灵渲染逻辑（billboard 替代 `gl_PointCoord`），包含混沌扰动/深度点大小/软边 glow/距离与 scale 的颜色&透明度调制
  - 使用实例化 billboard 四边形渲染粒子（加法混合），并通过 DrawIndirect（等效 `glDrawArraysIndirect`）验证“大规模粒子数据 -> GPU 缓冲 -> 渲染”链路
- 已推进（阶段 3 第 2 步：粒子 ComputeSaturn 物理模拟）：
  - 移植 OpenGL `ComputeSaturn` 为 Diligent Compute PSO（D3D12/HLSL + Vulkan/GLSL）
  - Compute 每帧读取当前粒子 buffer、写入下一 buffer，并进行三缓冲轮转（避免读写冲突）
- 阶段 3 已完成，原阶段 4（行星渲染）已从 OpenGL 版本移除，不再迁移。
- 已推进（阶段 4 后处理效果）：
  - 实现离屏 HDR 渲染流程（场景先渲染到 offscreenColor_ 纹理）
  - 实现 Kawase Blur ping-pong 渲染（1/6 分辨率，4 次迭代）
  - 集成 Bloom 效果到最终合成（全屏四边形混合原图 + 模糊）
  - Bloom 强度可调（通过 ImGui 滑块，0.0 ~ 2.0）
- 已推进（阶段 5 ImGui 集成）：
  - 创建自定义 ImGuiDiligent 渲染器（支持所有 Diligent 后端：D3D12、Vulkan 等）
  - 集成 imgui_impl_win32.cpp 处理 Win32 输入
  - 实现 FPS 显示（移动平均 60 帧，颜色随帧率变化：绿 ≥50、橙 30-50、红 <30）
  - Debug 窗口显示后端类型、分辨率、Bloom 强度滑块、模糊开关
- Vulkan 后端依赖显卡驱动提供 Vulkan 运行时（一般安装显卡驱动即可）；安装 Vulkan SDK 可让 CMake 检测与调试体验更好。

## 常见陷阱

### Shader Resource Variable 类型选择（MUTABLE vs DYNAMIC）

Diligent Engine 的 `ShaderResourceVariableDesc` 有三种类型，选择错误会导致资源绑定失效：

| 类型 | 设置时机 | 可修改次数 | 适用场景 |
|------|----------|------------|----------|
| `STATIC` | PSO 创建时通过 `GetStaticVariableByName()` | 1 次 | 全局常量缓冲（如 `ComputeConstants`） |
| `MUTABLE` | SRB 创建后通过 `GetVariableByName()` | **仅 1 次** | SRB 生命周期内固定的资源 |
| `DYNAMIC` | 每帧通过 SRB 的 `GetVariableByName()->Set()` | **每帧可更新** | 需要每帧切换的资源（如三缓冲轮转） |

**典型错误**：将需要每帧更换的缓冲区（如 Compute Shader 的输入/输出 buffer）声明为 `MUTABLE`。

```cpp
// ❌ 错误：MUTABLE 只能设置一次，后续 Set() 调用不生效
const ShaderResourceVariableDesc vars[] = {
    {SHADER_TYPE_COMPUTE, "g_ParticlesIn",  SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    {SHADER_TYPE_COMPUTE, "g_ParticlesOut", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
};

// ✅ 正确：DYNAMIC 允许每帧更换缓冲区
const ShaderResourceVariableDesc vars[] = {
    {SHADER_TYPE_COMPUTE, "g_ParticlesIn",  SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    {SHADER_TYPE_COMPUTE, "g_ParticlesOut", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
};
```

**症状**：Compute Shader 计算结果不生效（如粒子不移动），但无运行时错误。

**调试技巧**：在 Compute Shader 中强制修改输出颜色为固定值（如红色），如果渲染结果不变，说明 Compute 输出未被正确读取，检查变量类型是否应为 `DYNAMIC`。

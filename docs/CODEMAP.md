# CODEMAP — 代码库地图

> **写代码前先查这里。** 需要某个能力（接口/服务/工具/脚本）时：① 查本表 ② 全库 grep 确认 ③ 都没有才新建，并在**同一提交**把新东西登记到本表。
> 本表只描述"现在有什么、在哪、什么状态"；历史见 `MIGRATION_LOG.md`，理由见 `DECISIONS.md`（引用格式 D-xxx），债务明细见 `AUDIT_2026-07.md`（引用其第一部分编号 AUDIT P0-1~P3-7）。
> **过期地图比没有地图更害人**：移动/新增/删除模块时同一提交更新本表。

## 渲染路径（对比实验的被测对象；接缝以下保持原生写法，D-001/D-002）

| 路径 | 位置 | 状态 |
|---|---|---|
| macOS Metal | `src/gpu/backends/metal/` | 现役，拟定参考路径；object/mesh shader 为声明分歧（D-004）；已实现 GPU 设备契约 |
| macOS OpenGL 4.1 | `src/gpu/backends/opengl41/` | 现役；无 compute，粒子=变换反馈（默认）/解析式双策略；豁免 GPU 契约（D-002） |
| macOS Vulkan（MoltenVK / KosmicKrisp 双 ICD） | `src/gpu/backends/diligent/DiligentVulkanAdapter.*` | 现役；经 DiligentCore Vulkan；已实现 GPU 设备契约 |
| Windows OpenGL 4.3+ | `src/OpenGL/` | 冻结遗留（D-015）：保持可用，勿扩展 |
| Windows D3D11/D3D12/Vulkan | `src/Diligent/`（DiligentBackend.cpp，6221 行巨类） | 冻结遗留（D-015） |

## 共享层（只此一份——发现自己在写类似物 = 立即停下查重）

| 能力 | 位置 | 备注 |
|---|---|---|
| 应用状态/命令/帧协调 | `src/app/`（AppController、AppCommand、FrameCoordinator、state/） | 唯一现行状态模型；`src/AppState.h` 是旧模型，勿用 |
| GPU 设备契约 | `src/gpu/interface/`（GpuDevice.h、GpuTypes.h、GpuCapabilities.h） | 范围已冻结（D-002）：句柄/过渡/令牌/Dispatch/DrawIndirect；**勿扩表面积** |
| 渲染资源生命周期 | `src/render/`（ResourceRegistry/TexturePool） | pass 顺序已按 D-003 在各后端静态直排；RenderGraph 已删（2026-07-26） |
| 着色器 ABI 生成 | `src/shaders/abi/` + CMake 生成器 → `${CMAKE_BINARY_DIR}/generated/shaders` | 四语言结构唯一来源，禁止手写副本 |
| 粒子规范初始化 | `src/shaders/abi/ParticleInit.h` | 固定种子逐位确定的 CPU 端唯一事实来源；GL41 生产与后端测试共用，勿另写副本 |
| 着色器源码 | `src/shaders/{msl,glsl,glsl410,hlsl}/` | 共享通道单源化进行中（D-004，TODO P3） |
| 单源着色器（试点） | `src/shaders/single/ToneMap.hlsl` | 规范源；DXC→SPIR-V→SPIRV-Cross 产 GLSL410/MSL（工具链 2026-07-26 拍板） |
| MD3 UI 库 | `src/ui/md3/`（MD3.h、MD3Widgets、MD3Theme、MD3Context、MD3Shaders） | **唯一现行版**；`src/OpenGL/md3`、`src/Diligent/md3` 是待删旧拷贝（冻结区） |
| 日志 | `src/ui/md3/MD3Log.h` | 共享日志设施（重复合并/AddOnce/ANSI 剥离/级别检测）；macOS 端取代 `src/DebugLog.h` |
| 结构化诊断 | `src/services/diagnostics/`（DiagnosticBus） | 域/代码/级别/时间戳；UI 显示走 MacOSMd3Panel |
| 设置持久化 | `src/services/settings/`（macOS: NSUserDefaultsStore） | Windows 注册表实现在旧目标内，冻结 |
| 相机 | `src/services/camera/`（macOS: AVFoundationCamera + 原生选择窗口） | 纯 AVFoundation（D-011） |
| 手势推理 | `src/services/hand_tracking/`（XnnpackRuntime、HandTrackingWorker） | TFLite/XNNPACK；Windows 版在 `HandTracker/`（冻结） |
| SIMD 内核与调度 | `HandTracker/`（CpuFeatureDetector、KernelRegistry、KernelDispatcher、SIMDNormalize*） | 指令集范围封顶见 MIGRATION_LOG §13.2；勿加新指令集 |
| 资源定位 | `src/services/resources/` | Bundle / exe 目录 |
| Vulkan 驱动运行时 | `src/services/vulkan/` | VK_DRIVER_FILES 唯一 ICD、切换重启（D-013） |

## 入口

| 入口 | 位置 | 备注 |
|---|---|---|
| macOS 统一启动器 | `src/platform/macos/LauncherMain.mm`、MacOSBackendSelection.* | 读持久化 GraphicsApi/VulkanDriver 选路径 |
| macOS 三个模式 main | `src/platform/macos/{Main,OpenGL41Main,VulkanMain}.mm` | 已收口到 AppShell::RunApp：只剩后端构造 + renderFrame 闭包 + ImGui 接线 |
| 冒烟/基线统一支撑 | `src/platform/macos/SmokeHarness.{h,cpp}` | SmokeConfig 环境解析+状态钉死、启动几何、逐帧性能/全屏状态机；三 main 共用 |
| 唯一应用外壳 | `src/platform/macos/AppShell.{h,mm}`（RunApp + AppHost + FrameContext） | D-002 帧高度接缝：外壳管设置/相机/手势/输入/帧推进/FPS/面板/冒烟；三 main 全部经 renderFrame 回调接入；GL41 以 AppHost shim 保留自建窗口栈 |
| Cocoa 宿主/窗口 | `src/platform/macos/CocoaHost.*`、MacOSApplication.* | 全屏/材质/事件 |
| macOS MD3 调试面板 | `src/platform/macos/MacOSMd3Panel.*` | 三后端共用 |
| Windows 入口 | `src/Diligent/Main.cpp`、`src/OpenGL/Main.cpp` | 冻结 |

## 测试与基准

| 东西 | 位置 / 说明 |
|---|---|
| 测试全集 | `tests/`（ctest 注册散布于各 CMakeLists） |
| 对比模式 | `scripts/compare_macos_backends.sh` + `ParticleSaturnImageCompareTool`（tests/ImageCompareTool.cpp）：确定性捕获→并排/热力图/度量 |
| 跨后端粒子一致性 | `tests/CrossBackendParticleSystemTests.mm`（固定种子逐字段读回） |
| 视觉基线 | `tests/VisualBaselineMetricsTests.cpp`、`tests/RunMacOSVisualBaseline.sh`、`PARTICLESATURN_CAPTURE_BASELINE` 环境变量 |
| object shader 基线 | `tests/MetalObjectShaderBaselineTests.mm`（3e951b5 已修 argv/格式/半浮点读回） |
| 图像差异度量 | `tests/common/ImageMetrics.h` 唯一实现（阈值具名共享）；Windows CameraSelector 内旧份随 D-015 冻结 |
| 测试分层 | LABELS `unit`（无 GPU）/`gpu`（需设备）/`app`（整机 smoke，RUN_SERIAL）；顶层 CMake 强制：无标签即配置失败 |
| 测试基础设施保障 | 全部 `*Tests` 目标自动 `-UNDEBUG`（断言生效）；smoke 失败经 `CocoaHost::StopRunLoop` 传播非零退出码 + `[smoke] FAILED` 日志判负；哨兵 `tests/AssertSentinelTests.cpp` 防复发（均 2026-07-26，D-008） |
| 已知问题 | 4 个 FullscreenRestore smoke 显形为真失败，需真人前台复核（TODO 遗留人工验收） |

## 构建

| 东西 | 位置 | 状态 |
|---|---|---|
| 顶层 CMake | `CMakeLists.txt` | macOS 主构建入口 |
| 着色器编译 + ABI | `cmake/CompileShaders.cmake`（字节码头，需 DXC/FXC，**仅 Windows 段调用**）；macOS 走 `src/shaders/msl`/`glsl410` 子目录 + GenerateShaderAbi | 字节码头仍写入源码树 `src/generated/`（Windows 侧待迁） |
| 第三方补丁 | `patches/` + `scripts/apply_third_party_patch.*` | imgui-md3.patch 待重生成（D-007） |
| TFLite macOS | `scripts/build_tflite_macos.sh` | 锁 2.19，XNNPACK ARM64 |
| Windows 构建 | `*.vcxproj`、`scripts/*.ps1/.cmd`、`.github/workflows/release.yml` | 补丁应用已收敛到 apply_third_party_patch 单一入口（2026-07-26，待 push 验证） |
| macOS 测试 CI | `.github/workflows/macos-tests.yml` | push/PR 构建并运行 unit 层（待下次 push 验证） |

## 冻结区 — 勿修改、勿扩展、勿模仿

| 对象 | 原因 / 去向 |
|---|---|
| `src/OpenGL/md3/`、`src/Diligent/md3/` | MD3 旧拷贝，待删（TODO P2）；现行版 `src/ui/md3`；macOS include 路径存在 ODR 隐患（AUDIT P0-3） |
| `src/AppState.h/.cpp` | 旧状态模型；现行 `src/app/state/` |
| `src/OpenGL/`、`src/Diligent/`、`src/CameraSelector/`、`HandTracker/`（SIMD 调度除外） | Windows 现役但冻结（D-015）；重启时走窄接缝 |
| `src/Settings.h`、`src/ErrorHandler.h`、`src/DebugLog.h` | Windows 专属旧设施；macOS 对应物见共享层 |
| CMake FastRelease 配置 | 疑似死配置（AUDIT P2-5）；牵涉 Windows 构建，待 Windows 环境验证后删 |

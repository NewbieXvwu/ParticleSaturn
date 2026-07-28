# ParticleSaturn TODO

> **现行工作清单。** 历史与已完成：`docs/MIGRATION_LOG.md`（旧计划原文冻结归档，章节引用 §n）；架构决策：`docs/DECISIONS.md`（D-xxx）；代码库地图：`docs/CODEMAP.md`；技术债明细：`docs/AUDIT_2026-07.md`。
> **引用约定**：`AUDIT P0-1`~`P3-7` 指审计文档**第一部分**的条目编号（去重后的权威清单，含位置/量化影响/建议）；与本文件的阶段名 P0–P4 是两套编号，审计引用一律带 `AUDIT` 前缀。

> **状态（2026-07-27）**：P0 5/5、P1 5/5、P3 单源试点收束（tonemap+bloom 单源、场景着色器经逐 pass 数据定案不单源）、P4 四后端对比矩阵含逐 pass 指标跑通、性能速修全清。**P2 五项此前被错误地整体推迟到"Windows 环境"——已纠正（2026-07-27 用户裁定）**：每项拆为「代码实现+静态验证（本机必做）」与「Windows 构建/运行验证（遗留）」两段；本机段未完成前不得声称"macOS 可做项全清"。其余未勾项："遗留人工验收"节待真人真机；**"Windows 侧"节 D-015 重启已于 2026-07-28 启动（解冻），做满 Phase A+B+C，真机 `192.168.0.114` 可远程编译回验**。

## 冷启动协议（被要求"按 TODO 干活"时从这里开始）

1. 当前工作项 = **本文件 P0 节第一个未勾选项**（除非用户指名其他项）。阶段有依赖：P0 先于一切重构（没有可信的测试就没有安全的重构）；P1 是 P3/P4 的骨架；"性能速修"与各阶段无依赖、随时可做。
2. 动手前：读该项引用的 `AUDIT` 条目和 `D-xxx` 决策原文；按 CLAUDE.md 协议查 `docs/CODEMAP.md` 防止重复造轮子。
3. 一次一项：实现 → 在**断言生效的构建**下验证（D-008）→ 勾选并留一行备注 → 提交（提交信息引用条目）→ 有值得记录的细节则批注到 MIGRATION_LOG 文末"归档后进展"节。
4. **"遗留人工验收"节需要真人与真机**（真实摄像头、外接显示器、睡眠唤醒、肉眼看画面），agent 不得代做、不得标记完成，只能为其准备脚本与操作说明。
5. 标注（需拍板）的项先向用户提问，不要自行选定。
6. **缺环境 ≠ 不写代码（2026-07-27 用户裁定）**：涉及 Windows 的项必须先在本机完成全部代码实现与静态验证（vcxproj/CMake 一致性核对、diff 审阅、MSVC 兼容性人工检查、可在 macOS 编译的共享代码实机编译），只把"Windows 实机构建+运行"留作遗留验证段。禁止以"需 Windows 环境"为由整项搁置。

## P0 测试安全网（先于一切重构，D-008，AUDIT P0-2）

- [x] 全部测试目标断言生效：顶层 CMake 自动清扫全部 `*Tests` 可执行目标加 `-UNDEBUG`（新增测试目标自动覆盖，libs/ 第三方不受影响）；19/19 现役测试二进制恢复 assert 符号，18/18 非整机 ctest 真实通过，缺参运行由退出 0 变为断言中止。副作用断言保留原样——`-UNDEBUG` 保证其执行，防复发由哨兵测试兜底（下项）
- [x] smoke 测试失败可传播：`CocoaHost::StopRunLoop()`（`stop:` + 补发唤醒事件）取代 smoke/基线路径的 `terminate:`，main 的 `return 1` 复活；失败点统一打 `[smoke] FAILED` 日志并给全部 smoke 注册 `FAIL_REGULAR_EXPRESSION` 双保险。副作用：交互关窗现在正常走设置保存与清理。立即显形 4 个被掩盖的旧失败（见"遗留人工验收"全屏恢复条目）
- [x] ctest 自检：`tests/AssertSentinelTests.cpp` + WILL_FAIL——断言生效时 return 1 翻转为通过，断言被剥离时退出 0 翻转为失败报警；同时验证退出码传播。不用 assert(false)：信号型退出被 ctest 记 Exception，WILL_FAIL 不生效。负向验证：手动 -DNDEBUG 编译确认退出 0
- [x] ctest LABELS 分层：36/36 测试落层 unit 13 / gpu 7 / app 16（app 全部 RUN_SERIAL）；顶层 CMake 强制无标签即配置失败；`.github/workflows/macos-tests.yml` 跑 unit 层（unit 目标不链 DiligentCore，构建很轻）。CI 待下次 push 实际验证——本地领先 origin 226 提交，未代推
- [x] 修复 `.github/workflows/release.yml` 补丁逻辑：4 处 `git apply scripts/…`（路径全错，imgui 连文件名都错）收敛到 `sh scripts/apply_third_party_patch.sh` 单一入口；paths 过滤与 2 处缓存键同步指向 `patches/`；TFLite 现在正确应用双补丁（elementwise-compat 为标准 C++ 修正，MSVC 安全）。实际生效待下次 push
- [x] MetalObjectShaderBaselineTests 修复 argv/metallib、RGBA16F 格式、半浮点读回（3e951b5 完成）

## P1 接缝与外壳统一（对比实验室骨架，D-002/D-003/D-009）

- [x] 三个 macOS main 合并到唯一外壳 `AppShell::RunApp`：外壳独占设置/相机/手势/输入分发/帧推进/共享 FpsMeter（D-001 测量单份）/窗口镜像/材质与垂直同步/MD3 面板/冒烟/退出码；各 main 只剩后端构造 + renderFrame 闭包 + ImGui 接线（Metal 413→242 行、Vulkan 450→271、GL41 602→516 含保留的自建窗口栈）；GL41 经 AppHost shim 接入、窗口行为零改动。12/12 app + 7/7 gpu 通过（AUDIT P2-2）
- [x] smoke 逻辑抽出为 `src/platform/macos/SmokeHarness`（SmokeConfig 环境解析+状态钉死 / ResolveStartupGeometry / 逐帧性能与全屏状态机，宿主操作回调注入）；三 main 各删 ~90 行重复；失败统一打标+发 DiagnosticBus。未做 BUILD_TESTING 编译隔离——smoke 必须跑真实发布二进制（验收铁律），编译出去会让被测物偏离交付物；12/12 app 测试通过、全屏失败模式逐位一致（AUDIT P2-2）
- [x] `IRenderBackend` 窄接缝正名（2026-07-27）：AppShell.h 定义 `IRenderBackend`（Capabilities / RenderFrame / BaselineCaptured 读回面），三 main 各自以命名端点类（Metal/OpenGL41/VulkanRenderBackend）实现接入；基线捕获"落盘→退出"收束从三个 main 提到外壳一处；Vulkan adapter 误名 `BaselineCaptureRequested`（实返 captured）顺带正名。Init/Resize/Shutdown 留在各 main 对象生命周期，不强行入接口（D-002）
- [x] 渲染图静态化：三条 macOS 路径全部改为按书写顺序静态直排（原 Compile 输出可证恒等于插入顺序），Vulkan 的三个模糊链改参数化 lambda + for 循环；RenderGraph.{h,cpp} 无消费者后按 D-005 删除，RenderTests 缩减为 TexturePool 覆盖；unit/gpu/app 全绿 + 视觉基线逐像素（AUDIT P1-9，D-003）
- [x] 能力/特性协商单点：`BackendCapabilities` 在接缝申报（Metal 按 Metal3 管线实际可用性申报 objectShaderParticles，GL41 申报 analyticParticles），面板按能力显隐取代 `graphicsApi==Metal` 散判；声明分歧随申报登记并在 RunApp 启动发布到 DiagnosticBus 留档（D-004）。后续扩展点：GpuCapabilities 细粒度字段并入此处

## P2 单一事实来源与死代码清理（D-005/D-006）

- [ ] 删除 `src/OpenGL/md3/`、`src/Diligent/md3/`：Windows 目标改链 `src/ui/md3`（vcxproj 指向 + `_WIN32` 分支承接 Diligent 侧差异）。**macOS 侧 ODR 隐患已关闭**：ParticleSaturnMacOSImGui 上的 `src/OpenGL/md3` PUBLIC include 纯属遗留（其源码根本不含 MD3.h），已删，全量构建+gpu/app 测试通过（AUDIT P0-3 第一步）
  - [x] 本机段（2026-07-27 完成）：两目录（`src/OpenGL/md3`、`src/Diligent/md3`）已删；三目标统一编译 `src/ui/md3`；Diligent 侧差异（4 参 Init / void* 纹理 / Diligent PSO Ripple / stencil 圆角裁剪 / ApplyImGuiStyle / Acrylic UV 不翻转）以 `MD3_BACKEND_DILIGENT` 分支承接，GL 侧以 `MD3_HAS_OPENGL` 分支承接（互斥）；CMakeLists + 两 vcxproj 改指 `src/ui/md3` 并补 `src/ui` 使 `#include "md3/MD3.h"` 闭合；`.filters` 无 md3 条目无需改。**静态验证三重通过**：① macOS GL 路径 `ParticleSaturnMD3` 全绿；② 三份 .cpp 的 Diligent 分支对真实 Diligent 头 `clang -fsyntax-only` 零错（仅 Apple 平台头 `_countof` 宏重定义告警）；③ src/Diligent 下 37 个 `MD3::` 调用符号全部在统一 MD3.h 声明，签名逐一核对（Init 4 参 / SetBlurTexture(void*,bool)/2/Noise void* / ApplyImGuiStyle / DrawRipples / Shutdown / SetDpiScale）
  - [ ] Windows 验证段：两目标实机编译+运行+MD3 面板视觉确认
- [ ] `src/AppState.h` 旧状态模型处置：Windows 侧迁移到 `src/app/state/` 或显式冻结声明（AUDIT P1-4）
  - [x] 本机段（2026-07-28 完成）：择**显式冻结**而非迁移。盘点消费者：仅 src/OpenGL 与 src/Diligent 两个 Windows 目标经 `GetAppState/SetAppState`（GLFWwindow 用户指针）存取。判定迁移不可行——本模型（扁平全局 struct）携带 macOS 重设计 `ParticleSaturn::App::AppState` 刻意舍弃的 Windows 专属状态（DWM backdrop 材质、gl 崩溃报告信息、GLFW 窗口辅助、imguiInitialized 惰性标志、按键防抖 input、LOD 决策码），且字段命名体系全然不同（isDarkMode/enableBlur/handParams/window.isFullscreen vs darkMode/blurEnabled/gesture/window.fullscreen），二者是不同设计而非变体，合并将是跨 9 个 Windows 文件的大规模不可验证重写。已在 `src/AppState.h` 头部写入冻结声明 banner（D-002/D-005），显式标注不迁移理由并禁止新代码混用两模型。
  - [ ] Windows 验证段：实机编译确认
- [ ] CrashAnalyzer 两份合一（~620/630 行相同，已现分叉）；Win7Compat shim 两 vcxproj 共享同一 .cpp（AUDIT P2-7/P3-7）
  - [x] 本机段（2026-07-28 完成）：**CrashAnalyzer** 合一到共享 `src/CrashAnalyzer.h`（以 Diligent 版为基，GL 差异用 `MD3_BACKEND_DILIGENT` 承接：`CrashBlurTex` 类型别名统一 blur 句柄 ImTextureID/GLuint、Acrylic UV 的 Y 翻转分支；噪点判空统一为 `!=0`（void*/uint 通用）并去掉 `reinterpret_cast`（AddImageRounded 双重载按目标各自解析）；GL 侧顺带采用本地化 `str.*` 文案）；`src/OpenGL`、`src/Diligent` 两旧拷贝已删，OpenGL vcxproj 的 ClInclude 改指 `src\CrashAnalyzer.h`，Diligent 经 `src` include 目录解析。**Win7Compat** 收敛到唯一 `HandTracker/Win7Compat_GetSystemTimePreciseAsFileTime.cpp`（两份逻辑逐字节相同，HandTracker 版更全含 32 位 stdcall alias 且无 pch.h）；`src/` 拷贝已删，OpenGL vcxproj 改指 HandTracker 版并对全部 5 配置置 `NotUsing` PCH（免 pch.h 依赖），HandTracker.vcxproj 及其 DLL 构建零改动。静态闭合：grep 全库无代码残留引用旧路径。**注**：二者均 Windows-only（Windows.h/DbgHelp/`__imp_` 链接符号），macOS 无法编译验证，留待 Windows 段。
  - [ ] Windows 验证段：两目标实机编译+崩溃路径冒烟
- [x] 死代码批删（部分）：已删 `scripts/compile_shaders.ps1`（零引用）、`src/Diligent/SuperResolution.h`（仅自引用）、`MetalResourceManager`/`MetalCommandContext`（零消费者的 §8.1 骨架）、`NormalizeRGBRow`（无调用别名）。**保留**：CMake FastRelease 配置牵涉 Windows 构建本机无法验证（待 Windows 环境处理）；SIMD 的 SSE/AVX 枚举经查是 Windows 现役跨平台模式接口，非死代码
- [x] `src/gpu/interface/` 收敛至 D-002 冻结范围：删 7 个零消费的 §6.1 句柄标签/别名（TextureView/Sampler/ShaderModule/Pipeline/Binding*）、PresentMode 枚举、RequiredCapability+Supports+CapabilityName 三件套；GpuCapabilities 字段保留（后端在填）。unit/gpu 全绿（AUDIT P2-1）
- [x] `ParticleSimulationStrategy` 删除：无生产调用者，且其"按 GpuCapabilities 选模式"是 RHI 高度旧思路，与 D-002 帧高度接缝相悖——真实的策略协商归 P1 能力单点在接缝高度重建；ParticleSaturnGpu 随之改为纯头 INTERFACE 库（契约头即其全部内容）
- [x] 单实现服务接口去虚化：删 ICameraCapture 基类（Frame/Device/Authorization 等共享数据类型保留）与 SettingsStore.h（整文件即基类）；AVFoundationCamera/NSUserDefaultsStore 去 override，全库无任何多态使用点。unit + 相机/设置测试通过（AUDIT P2-1）
- [ ] 着色器字节码头生成改到 `${CMAKE_BINARY_DIR}/generated`，脱离源码树——Windows 专属流程：macOS 的 metallib/ABI 已在 build 目录；`src/generated/ShaderBytecodes.h` 由 Windows 流程生成、`src/Diligent/DiligentBackend.cpp` 引用（AUDIT P2-4）
  - [x] 本机段（2026-07-28 完成）：`CMakeLists.txt` 的 `SHADER_BYTECODES_HEADER` 输出改到 `${CMAKE_BINARY_DIR}/generated/ShaderBytecodes.h`；为 `ParticleSaturn.Diligent` 加 `target_include_directories` 首项 `${CMAKE_BINARY_DIR}/generated`；`DiligentBackend.cpp` 的引用由 `"../generated/ShaderBytecodes.h"` 改为 `"ShaderBytecodes.h"`（经 include 目录解析，`LogControlIcons.h` 仍是源码树 tracked 资产不动）；`.gitignore` 删除已废的 `src/generated/ShaderBytecodes.h` 条目（新位置在已忽略的 `bin_diligent/`、`build*/` 下）；vcxproj IntelliSense 路径 `DiligentIncludePaths` 首项补 `$(ProjectDir)bin_diligent\$(Platform)\$(Configuration)\generated`（CMAKE_BINARY_DIR 即 build 脚本传入的 BuildDir）。静态核对：全库无残留旧路径引用；源码树 `src/generated/` 仅剩 tracked 的 `LogControlIcons.h`，无生成物残留。与 AUDIT P2-4 建议逐条一致。
  - [ ] Windows 验证段：实机配置+编译确认生成物落在 build 目录
- [ ] `CompileShaders.cmake` 收敛：单一 compile_stage 函数取代 7 段复制、REGEX 取代逐字节 hex 循环——脚本无 DXC/FXC 即 FATAL、仅被顶层 CMake 的 Windows 段调用（AUDIT P2-4）
  - [x] 本机段（2026-07-28 完成）：`convert_to_byte_array` 的逐字节 `substring`+`APPEND` 内循环改为 REGEX 一次成型（按 16 字节/行分块，每块 `string(REGEX REPLACE "(..)" "0x\\1, ")`；CMake regex 不支持 `{n}` 量词故保留块级步进）；8 段近乎复制的 per-stage 代码块收敛为单一 `compile_stage(NAME LANG STAGE)` 函数（HLSL VS/PS/CS/MS/MeshPS + GLSL VS/PS/CS），映射源后缀/临时产物名/数组名/profile 或 glslang stage，就地 `PARENT_SCOPE` 累加 HEADER_CONTENT 与三计数器；驱动循环仅剩按原发射顺序（HLSL VS→PS→CS→MS→MeshPS，GLSL VS→PS→CS）的分派。计数语义逐段对齐旧版：常规段缺源 +SKIP/失败 +FAIL，MeshPS 尽力而为（缺源不 +SKIP、失败不 +FAIL），GLSL 段无 glslang 静默跳过不计。无 DXC/FXC 即 FATAL 与仅 Windows 段调用（顶层 CMake line 336 `return()` 门）均保持。**macOS 三重对拍验证**：① hex 转换独立 `cmake -P` 对拍旧/新实现，随机字节 N∈{1,2,15,16,17,31,32,33,100,255,1024} 及空文件全部逐位 MATCH；② `compile_stage` 直调测试确认 SKIP/FAIL/SUCCESS 计数、best-effort MeshPS、glslang 缺失跳过、数组名与 header append 均正确；③ 全脚本 `cmake -P` 词法解析通过，且旧版经同一 harness 在相同 `list(GET)` 点失败——证明我未引入新分歧（该 SHADERS 分词属既有行为，非本项范围）。
  - [ ] Windows 验证段：实机跑 DXC/FXC 全链路确认产物一致

## P3 着色器单源试点（D-004）

- [x] 工具链已拍板（2026-07-26，用户决定）：**DXC + SPIRV-Cross**，Slang 废止——决议记录于 D-004
- [x] tonemap 通道先行：单源产出 MSL/GLSL410/SPIR-V，接入三条 macOS 路径——**GL41 腿已完成**（2026-07-26）：`src/shaders/single/ToneMap.hlsl` 经构建期 DXC→SPIRV-Cross 产 `ToneMap.gen.frag`，GL41 换装（UBO+组合采样器）并同一提交删手写 frag；**量化**：替换前后 mean=0.000005、失配=0（逐像素等值），视觉基线/冒烟全过。**Vulkan 腿已完成**：adapter 直接消费 DXC 产出的 SPIR-V（Resources/single/ToneMap.spv），顺带消除了原内联源用线性采样器读场景的非故意分歧；量化：替换前后 mean=0.0038/失配 0.0025%，对 Metal 距离由 1.1762/0.359% 收敛到 1.1748/0.358%；Vulkan 全冒烟通过。**Metal 腿已完成**：ToneMapWithBloom compute 核改为全屏 fragment 渲染管线（生成的 main0 + 手写全屏三角 VS 样板进 metallib，PSO 按输出格式缓存 BGRA8/RGBA16F 两种）；量化：替换前后 mean=0.00297/失配 0.0022%（compute→光栅浮点微差），unit/gpu/app 全绿。**tonemap 通道三条路径单源化完成**
- [x] 推广 星空 → 粒子渲染（**bloom 站已完成**：三路径降采样/Kawase 模糊全部单源化，替换前后 GL41 0.001/Vulkan 0.154/Metal 0.005；Vulkan 顺带消除两处非故意分歧后对 Metal 失配 0.359%→0.027%，13 倍收敛；GL41 距离不变 → **分歧定位在场景 pass**。**2026-07-27 用户拍板：先逐 pass 仪表化**——数据已定案：星空/粒子三后端着色器算法逐行等价（模拟核、片元数学、混合态全同），GL41 的 3.62% 场景分歧 = 星空闪烁哈希吃 gl_FragCoord 窗口原点差（大幅度亮度差）+ 环区亚像素光栅化/超越函数实现差（8-32 LSB 大面积）——**全属 API 行为，场景着色器不单源化，两项已补进 declaredDivergences 保留观察**）
- [x] 声明分歧登记：随 BackendCapabilities.declaredDivergences 申报并于 RunApp 启动发布 DiagnosticBus（Metal object/mesh shader 手写 MSL、GL41 无 compute 双策略均已登记）
- [x] MIGRATION_LOG 归档后进展节已记录试点全程；CODEMAP 已登记 single/ 目录与工具链前置

## P4 对比模式（把测量做成功能）

- [x] `Readback` 正式纳入接缝签名（2026-07-27，随 IRenderBackend 正名完成：接缝 `BaselineCaptured()` 读回面 + 外壳统一收束，捕获机制保持各后端原生挂点）；确定性捕获现经 `PARTICLESATURN_CAPTURE_BASELINE`（固定种子/几何/暂停场景/锁 LOD]）在各后端可用，已被对比模式复用；逐 pass 捕获（`PARTICLESATURN_CAPTURE_PASS_DIR`）已完成（2026-07-27）：三后端导出 scene-hdr（全尺寸）与 bloom（1/6，泛光链终值）中间图，对比脚本逐 pass 出指标。**首组逐 pass 实测**：GL41 vs Metal 总帧 1.74/3.17%、scene-hdr 1.77/3.62%、bloom 0.12/0.000%；MoltenVK vs Metal 总帧 1.06/0.026%、scene-hdr 1.01/0.0004%、bloom 0.05/0.000% → **GL41 分歧 100% 在场景 pass，三后端后处理全部收敛**
- [x] 对比模式核心：`scripts/compare_macos_backends.sh` + `ParticleSaturnImageCompareTool`——同一确定性帧状态依次送各后端捕获，以 Metal 为参考输出并排图/差异热力图/共享度量。**首组实测**（2026-07-26）：GL41 vs Metal 均值差 1.74、失配 3.17%；MoltenVK vs Metal 均值差 1.18、失配 0.36%。**四后端矩阵补齐**（2026-07-27，含 KosmicKrisp 首组数据）：Kosmic vs Metal 与 Molten vs Metal 各 pass 全部同噪（scene-hdr 均 1.014/0.0004%）；Molten vs Kosmic 直接对比总帧 0.010/0.005%、scene 0.007/0%、bloom 0.001/0% → **双 ICD（D-013）近逐位一致，对 Metal 的 ~1 LSB 场景残差属 Vulkan 翻译路径共性而非驱动个性**
- [x] 图像差异度量收敛：两份 macOS 实现（视觉基线 PPM 版 / object shader 内存版，聚合语义核实相同）统一到 `tests/common/ImageMetrics.h` 累加器，阈值常量 PerPixelChannelThreshold=8 具名共享；第三份在冻结的 Windows CameraSelector（D-015 不动）。两组基线测试通过（AUDIT P2-9）
- [x] 粒子 CPU 参照两份合一：一份在 GL41 **生产**初始化、一份在 Metal 测试——归宿改为 `src/shaders/abi/ParticleInit.h`（ABI 旁的规范 CPU 端实现，生产与测试共用；tests/common 方向会让生产依赖测试）；Metal GPU 初始化对拍规范参照通过 = 抽取逐位一致（AUDIT P2-9）

## 性能速修（都在每帧热路径，彼此独立，随时可做；合集见 AUDIT P1-8/P2-8）

- [x] Metal 后处理四个类每帧重新加载 metallib 并重建 compute PSO → EnsurePipelines 惰性构建一次跨帧持有（对象提升为 MetalFrameRenderer 成员；原先每帧构造局部对象+在 pass lambda 里构造 acrylic）；调用方与测试签名零改动；gpu 层 7/7 + 视觉基线逐像素通过（AUDIT P1-8）
- [x] `AVFoundationCamera::LatestFrame` 锁内 ~1MB 深拷贝 → 消费语义下安全 `std::move`（b6802ce）
- [x] XnnpackRuntime 每帧分配输出 vector 与 147KB ROI 缓冲 → outputs_ 尺寸稳定后原地复用 + roiScratch_ 常驻成员（先跑 scripts/build_tflite_macos.sh 恢复了 /tmp 下的 TFLite 库才得以验证；3/3 手势测试通过）
- [x] DiagnosticBus 每 UI 帧全量深拷贝取一条 → `Latest()`/`SnapshotSince()` 稳态零拷贝 + deque 环形淘汰；`Snapshot()` 保留给测试断言（dd99248）
- [x] OpenGL41 逐绘制按字符串查 uniform → Initialize 缓存 GLint（Bloom/ToneMap/Present/StarField/七段 FPS/粒子模拟与渲染，Bloom 一帧省 ~68 次查找；088ec5e）

## 遗留人工验收（⚠️ 需要真人与真机，agent 勿代做勿勾选；验收定义原文见 MIGRATION_LOG）

> 引导脚本已备好（2026-07-27）：`scripts/manual_acceptance_macos.sh <app二进制> [编号...]`——逐项打印操作说明、代跑可自动化的部分、收集 PASS/FAIL 汇总；勾选仍需你亲手改本文件。

- [ ] Metal 成为 macOS 参考路径（终验，§8）
- [ ] MoltenVK、KosmicKrisp 分别完成可见画面、交互、设备丢失和重启后的运行验证（§10.6）
- [ ] 网格着色器对等路径实机验收（基线测试已修，待断言生效构建下重跑确认阈值）
- [ ] MD3 界面迁移视觉验收（功能项已勾，视觉对照保留）
- [ ] 窗口行为对齐；Retina 与外接显示器；睡眠唤醒（§16 阶段 10）
- [ ] **全屏恢复 smoke 真人复核**：退出码修复后 4 个 FullscreenRestore smoke（Metal/GL41/Molten/Kosmic）显形为真失败——转换等满 5 秒 deadline 未完成。证据指向环境因素（用户在别的应用活跃时 macOS 焦点保护拒绝后台 app 的 Space 切换；审计在旧 LastTest.log 早见过同样失败被记 Passed）。请在前台无干扰时跑 `ctest -R FullscreenRestore` 亲眼确认窗口是否正常进出全屏；若交互下也失败则是真 bug 需修
- [ ] 手势输入端到端验证（真实镜头：模型输出、关键点、丢手、旋转、缩放）
- [ ] 摄像头异常状态交互验收（拔线恢复、权限流程；无硬件部分已测）
- [ ] 四种 macOS 模式构建/启动/呈现/交互总验收；设置持久化、主题、材质、垂直同步、快捷键、LOD 行为对齐（交互部分）

## Windows 侧（D-015 重启进行中——2026-07-28 解冻，用户裁定做满 A+B+C）

> **重启已启动（2026-07-28 用户裁定）**：D-015 原文"Windows 后端未来作为 IRenderBackend 窄接缝实现接入"指的就是本节；启动此节 = 执行 D-015，而非推翻它。真机就位并验通：`192.168.0.114`（x64 / VS Community 2026 / Vulkan SDK 1.4.335 自带 dxc / DiligentCore 目录就位），经 SSH + PowerShell DefaultShell 通道可远程编译回验；仓库已同步到含 P2 成果的 `2f8c777`，`libs/imgui` 已回钉定 `a726bde`。
> **三决策已锁（2026-07-28）**：① 接缝中立化——`IRenderBackend/FrameContext/BackendCapabilities/BackendPanelHooks` 从 macOS `AppShell.h` 上提到平台中立头 `src/app/RenderSeam.h`（`DrawableSize`→`SurfaceSize`）；② 新单元落 `src/platform/windows/`（镜像 `src/platform/macos/`）；③ 状态模型收敛（Phase C）**纳入本次重启**——不留两套 `AppState`、不交第二次"重启税"（否认了此前的"触发式延后"，因为启动重启那一刻触发条件即成立，逻辑自我击穿）。
> **护栏（防"拆完重长"，AUDIT P0-4）**：`DiligentBackend.*` 编译期禁止 include 任何 window/settings/input/MD3 面板头，只允许接缝头 + Diligent + ImGui 绑定；加 CI grep 断言。每 Phase 结束跑对比模式(P4) + smoke，证明逐位不回归（验收铁律）。

- [ ] **Phase A — 拆分（项 98，D-009，AUDIT P0-4）**：建 `src/app/RenderSeam.h` 中立接缝，令 macOS `AppShell` 复用它（本机 Metal 先验证不回归）；`DiligentBackend.cpp`(6221 行) 瘦为纯 GPU 叶子（移除 `appState_/hwnd_/handTracker_` 所有权与 FPS/LOD/anim 成员，`RenderFrame` 改吃 `FrameContext`，实现 `Capabilities/BaselineCaptured`）；越界职责抽出到 `src/platform/windows/`：`Win32AppHost`（吞 Win32 窗口/全屏/DPI/backdrop + `Win32WindowManager`）、`Win32InputMapper`（WndProc 按键/尺寸/DPI → HostAction/AppCommand，不再直戳 AppState 字段）、`Services::Settings::Windows::RegistryStore`（收敛 `Settings.cpp`）、Windows composition root（`wWinMain` 瘦身到只构造+调 RunApp）
  - [x] **前置：Windows 首个绿色基线打通**（2026-07-28）——修复 `cmake/CompileShaders.cmake` 潜伏 bug（e51c8e1 引入：着色器列表用 `;` 分隔被 CMake 拍平成列表元素，`list(GET ... 1 ...)` 越界）：分隔符 `;`→`|` 且 `string(REPLACE "|" ";" ...)`。真机 Release 全量 209/209、着色器 Failed:0、`ParticleSaturn.Diligent.exe`(17199104 B) 产出。提交 `f42fec4`
  - [x] **Win32InputMapper 抽出**（2026-07-28，提交 `e4ac9ac`）——WndProc 的 WM_SETTINGCHANGE/KEYDOWN/KEYUP/SIZE/DPICHANGED 逐字移入 `src/platform/windows/Win32InputMapper.{h,cpp}` 的 `DispatchWindowMessage`；`Main.cpp` WndProc 瘦身为 ImGui 转发→委派 mapper→仅留 WM_DESTROY/default。真机验绿（BUILD_EXITCODE=0，exe 产出）。**注**：此步仍直戳 AppState 字段，尚未做 HostAction/AppCommand 语义化（留待 Phase C 收敛）
  - [x] `Settings.cpp` 注册表持久化平移到 `src/platform/windows/RegistryStore.cpp`（2026-07-28）——逐字搬移（`Settings` 命名空间与 API 不变），仅调整 include 相对路径；从 `add_executable` 源列表移除 `src/Diligent/Settings.cpp`、加入新路径。真机 Release 验绿（BUILD_EXITCODE=0，`[1/2]` 编译 RegistryStore.cpp、`[2/2]` 链接，exe 产出）。**注**：低层 `Reg*/CreateProcessW` 重启原语保留，Phase C 将其升为 `Services::Settings::Windows::RegistryStore` 类（仅高层 Save/Load 签名改吃 `App::AppState`）
  - [x] Windows 外壳自由函数抽出到 `src/platform/windows/Win32AppHost.{h,cpp}`（2026-07-28）——`IsDirectCompositionSupported / GetDpiScaleForWindow / ParseBackendFromCmdLine / GetClientSize` 从 `Main.cpp` 匿名命名空间逐字平移到 `ParticleSaturn::Platform::Windows`，`Main.cpp` 以 `using` 引入、`wWinMain` 不变。此文件将扩展为对应 macOS `CocoaAppHost` 的 Win32 AppHost。真机 Release 验绿（BUILD_EXITCODE=0，exe 产出）
  - [ ] 大块：FPS/LOD + FPS 曲线/七段面板深度纠缠 in-frame UI 渲染（读 `currentFps_/fpsHistory_`），叶子化需先做 D-002 面板所有权上移到外壳（drawPanel hook），与 Phase C 重叠。**注**：已确认 `DiligentBackend.cpp` 残存 `Settings::`（1096 LoadImGuiLayout / 1118 SaveSession / 4897 RestartWithBackend / 5227 SaveImGuiLayout）与 `handTracker_` 全部嵌在 init/shutdown/面板内，无独立可分离增量——以下为 A∧C 大移行本体（RenderFrame 改吃 FrameContext + 面板 drawPanel 上移 + 共享 AppController/FrameCoordinator）
- [ ] **Phase B — 三后端一致恢复（项 96，D-015）**：D3D11/D3D12/Vulkan（同一 `DiligentBackend` 靠 `Backend` enum 分支，非三个类）经外壳跑通并对齐；DComp / D3D11 原生 blit / Vulkan-D3D12 interop 透明路径不回归；接入对比模式(P4) 出行为基线
- [ ] **Phase C — 状态模型收敛**：Windows `AppState`（src/AppState.h 遗留全局模型）→ 共享 `App::AppState`（src/app/state/AppStates.h）；Windows 与 macOS 共用同一 `RunApp/AppController/FrameCoordinator`；平台特有窗口态（backdrop/material）留在各自 `AppHost` 之后
- [ ] 旧渲染器回归验证（承接阶段 2 尾项：状态拆分无回归）
- [ ] `patches/imgui-md3.patch` 从未格式化基线重生成（2.4MB→数百行，AUDIT P1-10，D-007）；真机 imgui 已备于钉定 `a726bde`

## 验收铁律（承袭 MIGRATION_LOG §17 并新增）

- 任何后端不得通过减少粒子、跳过通道或降低纹理尺寸取得通过结果
- 阈值在建立基线后固定，不得针对失败后端临时放宽
- 每项"完成"必须来自所选模式的真实呈现路径，不能以无表面设备或离线测试替代
- **新增**：所有测试结论必须来自断言生效的构建（D-008）；对比类结论必须附对比模式的量化指标

# ParticleSaturn TODO

> **现行工作清单。** 历史与已完成：`docs/MIGRATION_LOG.md`（旧计划原文冻结归档，章节引用 §n）；架构决策：`docs/DECISIONS.md`（D-xxx）；代码库地图：`docs/CODEMAP.md`；技术债明细：`docs/AUDIT_2026-07.md`。
> **引用约定**：`AUDIT P0-1`~`P3-7` 指审计文档**第一部分**的条目编号（去重后的权威清单，含位置/量化影响/建议）；与本文件的阶段名 P0–P4 是两套编号，审计引用一律带 `AUDIT` 前缀。

> **状态（2026-07-27）**：本机（macOS）可做项已全部完成——P0 5/5、P1 5/5、P2 macOS 侧全清、P3 单源试点收束（tonemap+bloom 单源、场景着色器经逐 pass 数据定案不单源）、P4 四后端对比矩阵含逐 pass 指标跑通、性能速修全清。未勾项仅剩三类：P2 五项需 Windows 环境改并验证；"遗留人工验收"节待真人真机；"Windows 侧"节按 D-015 冻结。下一步动作在用户：Windows 会话 / 前台人工验收 / 决定 push 触发 CI（本地领先远端约 260 提交）。

## 冷启动协议（被要求"按 TODO 干活"时从这里开始）

1. 当前工作项 = **本文件 P0 节第一个未勾选项**（除非用户指名其他项）。阶段有依赖：P0 先于一切重构（没有可信的测试就没有安全的重构）；P1 是 P3/P4 的骨架；"性能速修"与各阶段无依赖、随时可做。
2. 动手前：读该项引用的 `AUDIT` 条目和 `D-xxx` 决策原文；按 CLAUDE.md 协议查 `docs/CODEMAP.md` 防止重复造轮子。
3. 一次一项：实现 → 在**断言生效的构建**下验证（D-008）→ 勾选并留一行备注 → 提交（提交信息引用条目）→ 有值得记录的细节则批注到 MIGRATION_LOG 文末"归档后进展"节。
4. **"遗留人工验收"节需要真人与真机**（真实摄像头、外接显示器、睡眠唤醒、肉眼看画面），agent 不得代做、不得标记完成，只能为其准备脚本与操作说明。
5. 标注（需拍板）的项先向用户提问，不要自行选定。

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

- [ ] 删除 `src/OpenGL/md3/`、`src/Diligent/md3/`：Windows 目标改链 `src/ui/md3`（vcxproj 指向 + `_WIN32` 分支承接 Diligent 侧差异；需 Windows 环境验证）。**macOS 侧 ODR 隐患已关闭**：ParticleSaturnMacOSImGui 上的 `src/OpenGL/md3` PUBLIC include 纯属遗留（其源码根本不含 MD3.h），已删，全量构建+gpu/app 测试通过（AUDIT P0-3 第一步）
- [ ] `src/AppState.h` 旧状态模型处置：Windows 侧迁移到 `src/app/state/` 或显式冻结声明（AUDIT P1-4）
- [ ] CrashAnalyzer 两份合一（~620/630 行相同，已现分叉）；Win7Compat shim 两 vcxproj 共享同一 .cpp（AUDIT P2-7/P3-7）
- [x] 死代码批删（部分）：已删 `scripts/compile_shaders.ps1`（零引用）、`src/Diligent/SuperResolution.h`（仅自引用）、`MetalResourceManager`/`MetalCommandContext`（零消费者的 §8.1 骨架）、`NormalizeRGBRow`（无调用别名）。**保留**：CMake FastRelease 配置牵涉 Windows 构建本机无法验证（待 Windows 环境处理）；SIMD 的 SSE/AVX 枚举经查是 Windows 现役跨平台模式接口，非死代码
- [x] `src/gpu/interface/` 收敛至 D-002 冻结范围：删 7 个零消费的 §6.1 句柄标签/别名（TextureView/Sampler/ShaderModule/Pipeline/Binding*）、PresentMode 枚举、RequiredCapability+Supports+CapabilityName 三件套；GpuCapabilities 字段保留（后端在填）。unit/gpu 全绿（AUDIT P2-1）
- [x] `ParticleSimulationStrategy` 删除：无生产调用者，且其"按 GpuCapabilities 选模式"是 RHI 高度旧思路，与 D-002 帧高度接缝相悖——真实的策略协商归 P1 能力单点在接缝高度重建；ParticleSaturnGpu 随之改为纯头 INTERFACE 库（契约头即其全部内容）
- [x] 单实现服务接口去虚化：删 ICameraCapture 基类（Frame/Device/Authorization 等共享数据类型保留）与 SettingsStore.h（整文件即基类）；AVFoundationCamera/NSUserDefaultsStore 去 override，全库无任何多态使用点。unit + 相机/设置测试通过（AUDIT P2-1）
- [ ] 着色器字节码头生成改到 `${CMAKE_BINARY_DIR}/generated`，脱离源码树——经查为 **Windows 专属**：macOS 的 metallib/ABI 已在 build 目录；`src/generated/ShaderBytecodes.h` 由 Windows 流程生成、`src/Diligent/DiligentBackend.cpp` 引用，需 Windows 环境改并验证（AUDIT P2-4）
- [ ] `CompileShaders.cmake` 收敛：单一 compile_stage 函数取代 7 段复制、REGEX 取代逐字节 hex 循环——经查脚本无 DXC/FXC 即 FATAL、仅被顶层 CMake 的 Windows 段调用，本机无法执行验证，需 Windows 环境做（AUDIT P2-4）

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

## Windows 侧（冻结中，重启时按 D-015 走窄接缝）

- [ ] 旧渲染器回归验证（承接阶段 2 尾项：状态拆分无回归）
- [ ] Windows 三后端行为一致恢复——方法重定义：作为 IRenderBackend 实现接入，不经 RHI（D-015）
- [ ] `patches/imgui-md3.patch` 从未格式化基线重生成（2.4MB→数百行，AUDIT P1-10，D-007）
- [ ] `DiligentBackend.cpp`（6221 行）拆分——依赖方向规则（D-009）确立后进行，防止拆完重长（AUDIT P0-4）

## 验收铁律（承袭 MIGRATION_LOG §17 并新增）

- 任何后端不得通过减少粒子、跳过通道或降低纹理尺寸取得通过结果
- 阈值在建立基线后固定，不得针对失败后端临时放宽
- 每项"完成"必须来自所选模式的真实呈现路径，不能以无表面设备或离线测试替代
- **新增**：所有测试结论必须来自断言生效的构建（D-008）；对比类结论必须附对比模式的量化指标

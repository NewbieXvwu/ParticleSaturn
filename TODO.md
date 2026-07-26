# ParticleSaturn TODO

> **现行工作清单。** 历史与已完成：`docs/MIGRATION_LOG.md`（旧计划原文冻结归档，章节引用 §n）；架构决策：`docs/DECISIONS.md`（D-xxx）；代码库地图：`docs/CODEMAP.md`；技术债明细：`docs/AUDIT_2026-07.md`。
> **引用约定**：`AUDIT P0-1`~`P3-7` 指审计文档**第一部分**的条目编号（去重后的权威清单，含位置/量化影响/建议）；与本文件的阶段名 P0–P4 是两套编号，审计引用一律带 `AUDIT` 前缀。

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
- [ ] `IRenderBackend` 窄接缝正名：接缝已以 `RunAppConfig.renderFrame` + `FrameContext` + `AppHost` 形式存在且四路径全部接入；剩余是把它提升为命名接口并补 Capabilities（随下方能力单点项）与 Readback（随 P4）（D-002）
- [ ] 渲染图静态化：删除每帧 Compile/重建与拓扑排序（三后端每帧 30-45 次堆分配），pass 清单一次构建或静态直排，行为不变（AUDIT P1-9，D-003）
- [ ] 能力/特性协商单点：后端 `Capabilities()` 申报，特性开关在一处解析（`useObjectShader` 非 Metal 语义、GL41 无 compute 的粒子策略等），替代散落的后端 if；声明分歧在此登记（D-004）

## P2 单一事实来源与死代码清理（D-005/D-006）

- [ ] 删除 `src/OpenGL/md3/`、`src/Diligent/md3/`：Windows 目标改链 `src/ui/md3`（vcxproj 指向 + `_WIN32` 分支承接 Diligent 侧差异）；先移除 macOS include 路径上的旧 MD3.h（ODR 隐患）（AUDIT P0-3）
- [ ] `src/AppState.h` 旧状态模型处置：Windows 侧迁移到 `src/app/state/` 或显式冻结声明（AUDIT P1-4）
- [ ] CrashAnalyzer 两份合一（~620/630 行相同，已现分叉）；Win7Compat shim 两 vcxproj 共享同一 .cpp（AUDIT P2-7/P3-7）
- [ ] 死代码批删：`scripts/compile_shaders.ps1`（457 行，已被 CMake 版取代，AUDIT P1-11）、CMake FastRelease 死配置（AUDIT P2-5）、`src/Diligent/SuperResolution.h`（AUDIT P2-1）、SIMD 调度保留枚举/恒等分支/无调用 NormalizeRGBRow（AUDIT P1-3）、`MetalResourceManager`/`MetalCommandContext` 若已被设备契约取代
- [ ] `src/gpu/interface/` 清理至 D-002 冻结范围：删未消费的 GpuTypes 词汇类型与 GpuCapabilities 未用辅助（AUDIT P2-1）
- [ ] `ParticleSimulationStrategy` 接入真实策略选择或删除（ParticleSaturnGpu 库唯一源文件、无生产调用方；详见 AUDIT 第二部分 Medium）
- [ ] 单实现服务接口去虚化（ICameraCapture / SettingsStore 基类；保留共享数据类型）（AUDIT P2-1）
- [ ] 着色器字节码头生成改到 `${CMAKE_BINARY_DIR}/generated`，脱离源码树（AUDIT P2-4）
- [ ] `CompileShaders.cmake` 收敛：单一 compile_stage 函数取代 7 段复制、REGEX 取代逐字节 hex 循环（AUDIT P2-4）

## P3 着色器单源试点（D-004）

- [ ] （需拍板）选定工具链：DXC+SPIRV-Cross 或 Slang——向用户给出对比与推荐后由用户定
- [ ] tonemap 通道先行：单源产出 MSL/GLSL410/SPIR-V，接入三条 macOS 路径
- [ ] 用对比模式（P4）量化替换前后差异，通过后推广 bloom → 星空 → 粒子渲染
- [ ] 声明分歧登记：Metal object/mesh shader 保持手写 MSL（能力表记录）
- [ ] 完成后更新 MIGRATION_LOG §11 相关描述与 CODEMAP

## P4 对比模式（把测量做成功能）

- [ ] `Readback` 纳入接缝；固定种子 + 固定时间步长的确定性模式（复用 `PARTICLESATURN_CAPTURE_BASELINE` 基础）
- [ ] 同一帧状态送 2+ 后端离屏渲染：并排图、差异热力图、逐 pass 均值/失配率指标
- [ ] 三份图像差异度量实现收敛为 tests/common 一份，阈值常量具名共享（AUDIT P2-9）
- [ ] 两份逐字重复的粒子 CPU 参照实现（Metal/OpenGL 测试）抽到 `tests/common/ParticleReference.h`（AUDIT P2-9）

## 性能速修（都在每帧热路径，彼此独立，随时可做；合集见 AUDIT P1-8/P2-8）

- [x] Metal 后处理四个类每帧重新加载 metallib 并重建 compute PSO → EnsurePipelines 惰性构建一次跨帧持有（对象提升为 MetalFrameRenderer 成员；原先每帧构造局部对象+在 pass lambda 里构造 acrylic）；调用方与测试签名零改动；gpu 层 7/7 + 视觉基线逐像素通过（AUDIT P1-8）
- [x] `AVFoundationCamera::LatestFrame` 锁内 ~1MB 深拷贝 → 消费语义下安全 `std::move`（b6802ce）
- [x] XnnpackRuntime 每帧分配输出 vector 与 147KB ROI 缓冲 → outputs_ 尺寸稳定后原地复用 + roiScratch_ 常驻成员（先跑 scripts/build_tflite_macos.sh 恢复了 /tmp 下的 TFLite 库才得以验证；3/3 手势测试通过）
- [x] DiagnosticBus 每 UI 帧全量深拷贝取一条 → `Latest()`/`SnapshotSince()` 稳态零拷贝 + deque 环形淘汰；`Snapshot()` 保留给测试断言（dd99248）
- [x] OpenGL41 逐绘制按字符串查 uniform → Initialize 缓存 GLint（Bloom/ToneMap/Present/StarField/七段 FPS/粒子模拟与渲染，Bloom 一帧省 ~68 次查找；088ec5e）

## 遗留人工验收（⚠️ 需要真人与真机，agent 勿代做勿勾选；验收定义原文见 MIGRATION_LOG）

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

# ParticleSaturn TODO

> **现行工作清单。** 历史与已完成：`docs/MIGRATION_LOG.md`（旧计划原文冻结归档，章节引用 §n）；架构决策：`docs/DECISIONS.md`（D-xxx）；代码库地图：`docs/CODEMAP.md`；技术债明细：`docs/AUDIT_2026-07.md`（AUDIT #n）。
> **维护规则**：本文件只维护勾选状态与一行备注；进展批注写到 MIGRATION_LOG 文末"归档后进展"节。新增模块/接口/入口同一提交登记 CODEMAP。
> 阶段有依赖：P0 先于一切重构（没有可信的测试就没有安全的重构）；P1 是 P3/P4 的骨架；速修与人工验收随时可做。

## P0 测试安全网（先于一切重构，D-008）

- [ ] 全部测试目标断言生效：去 NDEBUG（`-UNDEBUG` / 测试专用编译选项）或引入 NDEBUG 免疫的 REQUIRE/CHECK 宏；副作用调用移出断言表达式。现状：21 个测试文件裸 `assert()` 在 Release 全部空转（AUDIT #40；3e951b5 已实证其掩盖了 4 个真 bug）
- [ ] smoke 测试失败可传播：`[NSApp terminate:nil]` 恒退出码 0，`return 1` 是死代码（CocoaHost.mm:195-197）→ 改 `[NSApp stop:]` + 显式退出码或失败即 `std::exit(1)`；ctest 加 `FAIL_REGULAR_EXPRESSION`（AUDIT #41）
- [ ] ctest 自检：注册一个故意失败的测试可执行文件，验证其确实以非零退出（防再度出现占位测试）
- [ ] ctest LABELS 分层：unit（无 GPU）/ gpu（需设备）/ app（整机 smoke，RUN_SERIAL）；GitHub Actions 增加 macOS unit job（AUDIT #44）
- [ ] 修复 `.github/workflows/release.yml` 四处复制的补丁应用逻辑（指向已迁移旧路径，发布流水线当前损坏）→ 收敛到 `scripts/apply_third_party_patch.*` 单一入口（AUDIT #28）
- [x] MetalObjectShaderBaselineTests 修复 argv/metallib、RGBA16F 格式、半浮点读回（3e951b5 完成）

## P1 接缝与外壳统一（对比实验室骨架，D-002/D-003/D-009）

- [ ] 合并三个 macOS main（Main.mm / OpenGL41Main.mm / VulkanMain.mm）为唯一 `RunApp(backendFactory)`；各 main 只留设备/表面构造（AUDIT #24）
- [ ] 内嵌在生产 main 里的 8 环境变量 smoke 逻辑（约 150 行 ×3）抽出为 SmokeHarness（AUDIT #45）
- [ ] 定义 `IRenderBackend` 窄接缝（Init / Capabilities / Resize / RenderFrame / Readback / Shutdown）；四条 macOS 路径以包装类接入，内部实现不改
- [ ] 渲染图静态化：删除每帧 Compile/重建与拓扑排序（三后端每帧 30-45 次堆分配），pass 清单一次构建或静态直排，行为不变（AUDIT #14，D-003）
- [ ] 能力/特性协商单点：后端 `Capabilities()` 申报，特性开关在一处解析（`useObjectShader` 非 Metal 语义、GL41 无 compute 的粒子策略等），替代散落的后端 if；声明分歧在此登记（D-004）

## P2 单一事实来源与死代码清理（D-005/D-006）

- [ ] 删除 `src/OpenGL/md3/`、`src/Diligent/md3/`：Windows 目标改链 `src/ui/md3`（vcxproj 指向 + `_WIN32` 分支承接 Diligent 侧差异）；先移除 macOS include 路径上的旧 MD3.h（ODR 隐患，AUDIT #12/#13/#34）
- [ ] `src/AppState.h` 旧状态模型处置：Windows 侧迁移到 `src/app/state/` 或显式冻结声明（AUDIT #26）
- [ ] CrashAnalyzer 两份合一（~620/630 行相同，已现分叉）；Win7Compat shim 两 vcxproj 共享同一 .cpp（AUDIT #23/#26）
- [ ] 死代码批删：`scripts/compile_shaders.ps1`（457 行，已被 CMake 版取代）、CMake FastRelease 死配置、`src/Diligent/SuperResolution.h`、SIMD 调度保留枚举/恒等分支/无调用 NormalizeRGBRow、`MetalResourceManager`/`MetalCommandContext` 若已被契约取代（AUDIT #30/#31/#17/#18）
- [ ] `src/gpu/interface/` 清理至 D-002 冻结范围：删未消费的 GpuTypes 词汇类型与 GpuCapabilities 未用辅助（AUDIT #15）
- [ ] `ParticleSimulationStrategy` 接入真实策略选择或删除（AUDIT #16）
- [ ] 单实现服务接口去虚化（ICameraCapture / SettingsStore 基类；保留共享数据类型）（AUDIT #25）
- [ ] 着色器字节码头生成改到 `${CMAKE_BINARY_DIR}/generated`，脱离源码树（AUDIT #35）
- [ ] `CompileShaders.cmake` 收敛：单一 compile_stage 函数取代 7 段复制、REGEX 取代逐字节 hex 循环（AUDIT #32）

## P3 着色器单源试点（D-004）

- [ ] 选定工具链（DXC+SPIRV-Cross 或 Slang），tonemap 通道先行：单源产出 MSL/GLSL410/SPIR-V，接入三 macOS 路径
- [ ] 用对比模式（P4）量化替换前后差异，通过后推广 bloom → 星空 → 粒子渲染
- [ ] 声明分歧登记：Metal object/mesh shader 保持手写 MSL（能力表记录）
- [ ] 完成后更新 §11 四套入口的描述性文档与 CODEMAP

## P4 对比模式（把测量做成功能）

- [ ] `Readback` 纳入接缝；固定种子 + 固定时间步长的确定性模式（复用 PARTICLESATURN_CAPTURE_BASELINE 基础）
- [ ] 同一帧状态送 2+ 后端离屏渲染：并排图、差异热力图、逐 pass 均值/失配率指标
- [ ] 三份图像差异度量实现收敛为 tests/common 一份，阈值常量具名共享（AUDIT #48）
- [ ] 两份逐字重复的粒子 CPU 参照实现（Metal/OpenGL 测试）抽到 tests/common/ParticleReference.h（AUDIT #43）

## 性能速修（都在每帧热路径，彼此独立，随时可做）

- [ ] Metal 后处理四个类每帧重新加载 metallib 并重建 compute PSO → Initialize 一次持有（MetalBackend.mm ToneMapper/Bloom/Acrylic/SevenSegmentFps，AUDIT #11）
- [ ] `AVFoundationCamera::LatestFrame` 锁内 ~1MB 深拷贝 → `std::move`/交换缓冲（AUDIT #19）
- [ ] XnnpackRuntime 每帧分配输出 vector 与 147KB ROI 缓冲 → 常驻成员复用（AUDIT #20）
- [ ] DiagnosticBus 每 UI 帧全量深拷贝取一条 → `Latest()` 接口 + 环形缓冲（AUDIT #21）
- [ ] OpenGL41 后处理每次绘制按字符串查 uniform 位置 → Initialize 缓存 GLint（AUDIT #27）

## 遗留人工验收（承接旧计划阶段 9/10，验收定义原文见 MIGRATION_LOG）

- [ ] Metal 成为 macOS 参考路径（终验，§8）
- [ ] MoltenVK、KosmicKrisp 分别完成可见画面、交互、设备丢失和重启后的运行验证（§10.6）
- [ ] 网格着色器对等路径实机验收（基线测试已修，待断言生效构建下重跑确认阈值）
- [ ] MD3 界面迁移视觉验收（功能项已勾，视觉对照保留）
- [ ] 窗口行为对齐；Retina 与外接显示器；睡眠唤醒（§16 阶段 10）
- [ ] 手势输入端到端验证（真实镜头：模型输出、关键点、丢手、旋转、缩放）
- [ ] 摄像头异常状态交互验收（拔线恢复、权限流程；无硬件部分已测）
- [ ] 四种 macOS 模式构建/启动/呈现/交互总验收；设置持久化、主题、材质、垂直同步、快捷键、LOD 行为对齐（交互部分）

## Windows 侧（冻结中，重启时按 D-015 走窄接缝）

- [ ] 旧渲染器回归验证（承接阶段 2 尾项：状态拆分无回归）
- [ ] Windows 三后端行为一致恢复——方法重定义：作为 IRenderBackend 实现接入，不经 RHI（D-015）
- [ ] `patches/imgui-md3.patch` 从未格式化基线重生成（2.4MB→数百行，D-007）
- [ ] DiligentBackend.cpp（6221 行）拆分——依赖方向规则（D-009）确立后进行，防止拆完重长（AUDIT #1/#5）

## 验收铁律（承袭 §17 并新增）

- 任何后端不得通过减少粒子、跳过通道或降低纹理尺寸取得通过结果
- 阈值在建立基线后固定，不得针对失败后端临时放宽
- 每项"完成"必须来自所选模式的真实呈现路径，不能以无表面设备或离线测试替代
- **新增**：所有测试结论必须来自断言生效的构建（D-008）；对比类结论必须附对比模式的量化指标

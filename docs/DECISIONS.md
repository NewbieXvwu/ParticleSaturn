# DECISIONS — 架构决策记录

> 只增不删：推翻旧决策 = 追加新条目并在旧条目标注"已废止（被 D-xxx 取代）"，不得默默绕过。
> Agent 在做任何方向性选择前先查本表，**不得重新引入已废止方案**。
> 引用格式：`D-002`；旧计划原文见 `docs/MIGRATION_LOG.md`（引用其章节号 §n）。

| 编号 | 日期 | 状态 | 一句话 |
|---|---|---|---|
| D-001 | 2026-07-26 | 有效 | 项目定位：渲染路径对比实验室，八目标矩阵保留 |
| D-002 | 2026-07-26 | 有效 | 接缝定在帧高度；GPU 设备契约范围冻结；GL41 豁免 |
| D-003 | 2026-07-26 | 有效 | 渲染图静态化，废除每帧重建+拓扑排序 |
| D-004 | 2026-07-26 | 有效 | 共享通道着色器单源试点；实验路径手写=声明分歧 |
| D-005 | 2026-07-26 | 有效 | 迁移纪律：替换必删旧 |
| D-006 | 2026-07-26 | 有效 | 无第二消费者不建抽象 |
| D-007 | 2026-07-26 | 有效 | 第三方只读边界 |
| D-008 | 2026-07-26 | 有效 | 测试安全网为架构级要求 |
| D-009 | 2026-07-26 | 有效 | 依赖方向规则 |
| D-010 | 2026-07-16 | 有效 | macOS HandTracker 静态链接 |
| D-011 | 2026-07-16 | 有效 | macOS 纯 AVFoundation，无 OpenCV |
| D-012 | 2026-07-16 | 有效 | KosmicKrisp 不锁提交自由追更 |
| D-013 | 2026-07-16 | 有效 | 双 Vulkan ICD 并列选择，不自动回退 |
| D-014 | 2026-07-16 | 有效(修订) | 不做事项清单（着色器项按 D-004 修订） |
| D-015 | 2026-07-26 | 有效 | Windows 统一路线重定义，RHI 化路线废止 |
| D-016 | 2026-07-26 | 有效 | 五文件文档体系与防失忆协议 |

---

## D-001 项目定位：渲染路径对比实验室（2026-07-26，有效）

本项目是实验/玩具项目，目的是**观察同一场景在不同渲染路径下的行为差异**。八目标矩阵（macOS: Metal / OpenGL 4.1 / Vulkan-MoltenVK / Vulkan-KosmicKrisp；Windows: OpenGL / D3D11 / D3D12 / Vulkan）全部保留——矩阵本身就是目的，不做削减。

**推论（实验有效性要求）**：除后端外一切共享——模拟语义、帧构成、参数、着色器逻辑（除声明分歧）、测量方法必须只有一份。路径间的差异必须要么是 API/驱动行为（信号），要么是登记在案的故意分歧（见 D-004），不允许意外漂移（噪音）。一切共享化工作服务于让差异可信、可测量。

## D-002 接缝定在帧高度；GPU 设备契约范围冻结；GL41 豁免（2026-07-26，有效）

后端统一经**窄接口**接入唯一应用外壳：`Init / Capabilities / Resize / RenderFrame(共享帧描述) / Readback / Shutdown` 量级，约 6 个方法。接缝以上绝对统一（实验设置），接缝以下各后端保持最地道的原生写法（实验对象）。**不建 RHI**——RHI 级抽象会抹掉要观察的 API 差异，且工程量与项目目的不符。

`src/gpu/interface/GpuDevice.h` 的共享设备契约以 2026-07-26 实际落地范围为**最终范围**：代际校验缓冲句柄（槽位复用/失效检测）、范围受控更新、显式用途状态过渡（Transition）、`Dispatch`/`DrawIndirect`、`BeginCommands`/`Submit` 提交令牌与延后释放。该范围已由 `DiligentVulkanAdapter` 与 `MetalDevice`（commit 8098fc6）实现。原计划 §6.1 的其余表面积——GpuInstance/GpuAdapter、Texture/Sampler/TextureView 抽象、ShaderModule/ShaderReflection、BindingLayout/BindingSet、Render/Compute/Copy Encoder 分层、Fence——**不再建设**。

**OpenGL 4.1 后端豁免接入契约**：GL 没有真实的命令列表与资源状态语义，强行实现契约是虚构，无实验价值。

**废止**：MIGRATION_LOG §6.1 对象大表中未落地部分；§6.4"RenderGraph、GpuDevice 和资源注册表必须进入 Metal、OpenGL 4.1、Vulkan 的实际帧路径，否则迁移项保持未完成"的强制条款；阶段 3 的"DiligentAdapter 全面 RHI 化"路线。

## D-003 渲染图静态化（2026-07-26，有效）

废除每帧重建图 + 拓扑排序的执行机制：三条 macOS 路径每帧付出约 30-45 次堆分配（字符串、std::function、vector），而编译结果恒等于书写顺序。共享 **pass 清单**作为概念与数据保留（一次构建，或静态直排 `if (!DoPass(...)) return false;`），后端照单执行，行为不变。

**修订**：MIGRATION_LOG §7.2"生产帧必须由该图执行通道顺序……各后端手写串行调用只能作为接入前的临时路径"的验收条款。资源生命周期/缩放重建逻辑保留（已验收），只去掉每帧图编译。

## D-004 共享通道着色器单源试点；实验路径手写=声明分歧（2026-07-26，有效）

共享后处理通道（从 tonemap、bloom 起步）试点**单源交叉编译**：HLSL 经 DXC→SPIR-V→SPIRV-Cross 产出 MSL 与 GLSL 410（或采用 Slang），用对比模式量化替换前后的画面差异，逐通道推广。单源后各路径的着色器差异纯粹来自编译器/驱动行为，信号更干净。

作为实验对象的路径（当前：Metal object/mesh shader 粒子路径）**保留手写 MSL**，在能力/技术登记中记录为**声明分歧**——它们是故意的实验变量，不是漂移。

**2026-07-26 拍板**（用户决定）：工具链定为 **DXC + SPIRV-Cross**；Slang 备选废止。理由：SPIRV-Cross/glslang 已随 DiligentCore 在库、零新依赖；现有 `src/shaders/hlsl` 源直接复用；MoltenVK 本身经 SPIRV-Cross 生成 MSL，翻译层与现役路径同源，对比信号更干净。

**部分废止**：2026-07-16 决策"正式 Metal 包只加载手写 MSL 生成的 metallib，SPIRV-Cross 仅开发期对照"（MIGRATION_LOG §8.4、§11.4、§20 第 9 条）——对声明分歧路径继续有效，对共享通道废止。

## D-005 迁移纪律：替换必删旧（2026-07-26，有效）

任何迁移/提取/替换必须在**同一提交系列**内删除旧实现。确实暂不能删的，登记进 `docs/CODEMAP.md` 冻结区并注明原因与删除条件。半途迁移是最危险状态：双份维护 + 漂移 + ODR 隐患。

历史教训（见 AUDIT_2026-07 第一部分）：MD3 三份拷贝（P0-3）、AppState 双模型（P1-4）、CrashAnalyzer 双份（P2-7）、Win7Compat 双份（P3-7）、compile_shaders.ps1 尸体（P1-11）。

## D-006 无第二消费者不建抽象（2026-07-26，有效）

接口/抽象层在第二个真实消费者出现之前不得新建。需要某能力先查 CODEMAP + 全库 grep。历史教训：GpuDevice 大表停滞十日（后按 D-002 收窄范围才落地）、TexturePool 零池化使用（AUDIT P1-9）、单实现服务接口（AUDIT P2-1）。

## D-007 第三方只读边界（2026-07-26，有效）

第三方子模块代码**只读**；扩展走官方扩展点（imconfig、回调、继承）或干净的 fork 分支，不就地改写。`patches/imgui-md3.patch`（2.4MB，约 95% 为整文件重排格式噪音）需从未格式化的上游基线重新生成为数百行真实差异；DiligentCore 本地修改收敛为 `patches/` 下的受控补丁。否则每次上游升级都是大出血，理性结局是永不升级。

## D-008 测试安全网为架构级要求（2026-07-26，有效）

1. 所有测试必须在**断言生效**的构建下运行（测试目标去 NDEBUG，或改用 NDEBUG 免疫的 REQUIRE/CHECK 宏）；
2. smoke 测试失败必须以**非零退出码**传播（`[NSApp terminate:nil]` 恒为 0，`return 1` 是死代码——改 `[NSApp stop:]` + 显式退出码或 `std::exit(1)`）；
3. 每个后端支持 headless/离屏/固定种子确定性渲染与读回，作为接缝（D-002）的一部分；
4. CI 至少运行 unit 层测试（ctest LABELS 分层：unit / gpu / app）。

**实证**：commit 3e951b5 自述"the registered Release test was a no-op because NDEBUG strips assert"，真跑基线后修出 object shader 路径 4 个真 bug（错误像素格式与混合、payload 竞态致仅 1/32 粒子被绘制、mesh 缓冲未绑定、测试自身忽略 argv/传 nullptr）。占位测试的代价是真实的。

## D-009 依赖方向规则（2026-07-26，有效）

`platform → app(core) → services` 单向依赖；渲染后端是叶子——**不得**内嵌 UI 面板创建、输入处理、手势集成（旧 DiligentBackend 6221 行的成因）。应用本体定义收口到唯一外壳（RunApp/composition root），不散落在平台 main 文件里。继承 MIGRATION_LOG §3 的依赖规则并加严。

## D-010 macOS HandTracker 静态链接（2026-07-16，有效，承袭）

理由四条（原文见 MIGRATION_LOG §12.5）：避免 @rpath/签名复杂性；消除卸载线程竞态（Windows 已因此注释 "Do NOT FreeLibrary"）；LTO 跨库优化热路径；无动态分发需求。Windows 保持 LoadLibraryW + DLL 不变。

## D-011 macOS 纯 AVFoundation + Accelerate/vImage，无 OpenCV（2026-07-16，有效，承袭）

消除约 500MB 子模块与许可证约束；`CVPixelBuffer` 直达推理张量。原文见 MIGRATION_LOG §12.3。

## D-012 KosmicKrisp 不锁提交，自由追更（2026-07-16，有效，承袭）

玩具项目不需要稳定性承诺；驱动异常必须在自己后端内暴露，不得静默切换 MoltenVK。原文见 MIGRATION_LOG §10.4。

## D-013 双 Vulkan ICD 并列选择，不自动回退（2026-07-16，有效，承袭）

任何 Vulkan 调用前经 `VK_DRIVER_FILES` 指定唯一 ICD；切换后重启。两 ICD 不同时枚举、失败不自动换用。原文见 MIGRATION_LOG §10.1。

## D-014 不做事项清单（2026-07-16，有效，修订，承袭）

MIGRATION_LOG §20 清单继续有效：不用 DiligentCorePro / bgfx / SDL GPU / Dawn / wgpu；注册表不迁 JSON；不做签名公证安装器；不对整目标 `-march=native`；不一次性重写 Windows。**修订**：第 9 条"不通过 SPIRV-Cross 生成正式 Metal 着色器"按 D-004 改为仅适用于声明分歧路径。

## D-015 Windows 统一路线重定义（2026-07-26，有效）

废止旧计划阶段 1（行为基线截图工程）、阶段 3（DiligentAdapter RHI 化）、阶段 4"Windows 三个 Diligent 后端全部恢复一致"的原方法。Windows 后端未来作为 IRenderBackend 窄接缝（D-002）下的普通实现接入；行为基线由对比模式（TODO P4）承担。在此之前 `src/OpenGL/`、`src/Diligent/`、`src/CameraSelector/`、`HandTracker/` 冻结：保持可用，勿重构、勿扩展。

## D-016 五文件文档体系与防失忆协议（2026-07-26，有效）

`TODO.md`（未来）/ `docs/MIGRATION_LOG.md`（过去）/ `docs/DECISIONS.md`（永远）/ `docs/CODEMAP.md`（现在）/ `docs/AUDIT_2026-07.md`（债务快照）。维护规则见 CLAUDE.md "Codebase Documentation Protocol"。**文档不得增殖**：新增任何常设文档需在本表登记决策；过期文档比没有文档更害人，每份文档必须有让它保鲜的规则。

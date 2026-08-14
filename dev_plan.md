# dev_plan：v2.2 转正与可用化（feat/mj2x-v2.2）

> 依据 issue #787 §3（mj2x 部分）。两条主线：**RegC 性能基线**（P1，贯穿）+
> **VMC4 虚拟机后端可用版**（P2，最大工程）。本计划是 v2.2 mj2x 的施工依据，
> 每 Phase 完成后单独提交，状态表同步更新。

## 现状对照

| 主线 | 已有 | 缺口（v2.2 要做） |
|---|---|---|
| RegC 性能 | `performance_evaluation_spec.md` 方法学；NativeBench 3 基准（fib/loopSum/arraySum）；JDK intrinsic 大量覆盖 | 对比 JNIC 基线；对象/字段/调用/数组/intrinsic 场景基准；JDK 17/21 矩阵；方法内联；System.arraycopy 等更宽 intrinsic |
| VMC4 可用版 | Phase 0-5：5 dispatcher、opcode/handler 随机化、per-class/method 粒度、ALT 别名、super-op、bogus、drift、trace surface；纯计算+控制流 | A 对象/字段/数组、B 调用/虚分派、C 异常语义、D intrinsic 对拍（当前全 fail-closed） |

## 状态总览

| Phase | 主线 | 内容 | 状态 |
|---|---|---|---|
| 2.0 | VMC4 | Spike：REF 类型 + 调用链最小闭环 + 决策 handler 复用方案 | ✅ 完成 |
| 2.A | VMC4 | LoadField/StoreField/ArrayLoad/ArrayStore/Allocate | ✅ 完成 |
| 2.B | VMC4 | Call（virtual/special/static/interface + cache） | ✅ 完成 |
| 2.C | VMC4 | Throw + 异常区域编码 | ✅ 完成 |
| 2.D | VMC4 | Intrinsic 子集 + 双后端对拍 | ✅ 完成 |
| 2.E | VMC4 | 可用化收口（真实 jar 转化率 + e2e 扩展 + trace surface） | ✅ 完成 |
| 1.1 | RegC 性能 | 基准套件扩展（含 JNIC 对比 + JDK 17/21） | ✅ 完成 |
| 1.2 | RegC 性能 | 针对性优化（intrinsic 扩面 / 通用 helper 收敛 / 内联评估） | 部分完成 |

## 执行顺序与依赖

```
VMC4（主线二，串行强依赖，issue 要求"开头 spike"）
  2.0 Spike ── 决策 handler 复用方案 + CRT 共享机制（阻塞后续全部）
       └─ 2.A 对象/字段/数组
            └─ 2.B 调用/虚分派
                 └─ 2.C 异常（工程难度最大）
                      └─ 2.D intrinsic 对拍
                           └─ 2.E 收口

RegC 性能（主线一，P1，可与 VMC4 并行，按数据驱动优化）
  1.1 基准套件（含 JNIC）── 先做，产出数据
       └─ 1.2 优化（按 1.1 数据决策内联是否必要）
```

## 主线二：VMC4 可用版（P2，最大工程）

### 关键技术决策：handler 复用方案

VMC4 扩展语义 family 时，handler 复用 RegC 已验证的 `_mj2x_regc_*` C ABI helper
（方案 A），不独立重写（方案 B）。理由：RegC helper（3818 行 emitter）已覆盖
field/call/array/alloc/throw 全语义且经 `-Xverify:all` E2E 验证，复用可直接
继承语义正确性。前置工作：把 RegC runtime prelude 抽成 VMC4 解释器可 include 的
共享片段（2.0 Spike 落实）。

> **2.0 Spike 落地修正（2026-08-13）**：Call family 未走方案 A 的 RegC helper 复用，
> 而是采用了 **per-callsite 独立 JNI helper**（方案 B 的变体）。原因：RegC 的
> `_mj2x_regc_call_*` 是 varargs + 全局 `_mj2x_regc_env` 形态，而 VMC4 的参数在
> `regs[argIdx[i]]`（寄存器索引数组），两者签名不兼容；且 VMC4 既有设计明确
> "不依赖 RegC CRT"（vmc4_backend.cpp:46）。落地形态：每个 VmMethodRef 发射一个
> `mj2x_vm_call_<tag>_<idx>` TU-local helper（懒缓存 class/methodID + 按 argKinds
> marshalling + Call[Static]<Type>MethodA），解释器经 `mj2x_vm_call_table_<tag>[]`
> 函数指针表分派。后续 2.A 的 field/array 若同样与 RegC helper 签名不兼容，沿用此
> per-site helper 模式；2.A 起再评估是否抽共享 marshalling 片段。

### Phase 2.0 — Spike：对象模型 + 调用链最小闭环 ✅

**目标**：验证 REF 类型承载 jobject、Call 调用链端到端跑通。此 Phase 决策后续全部
VMC4 工作的落地形态。

- REF 类型建模：`mj2x_vm_slot` 加 `void* ref` 承载 jobject（slot.ref ↔ jobject 双向 cast）
- Call 编码：`encodeCall` 把 invokestatic/virtual/special/interface 收集为 `VmMethodRef`
  （含 argKinds/retKind），字节流存 callSiteIdx；instance 调用 receiver 取 args[0]
  （SSA 约定），描述符参数从 args[1..]
- Call 发射：per-callsite `mj2x_vm_call_<tag>_<idx>` helper（懒缓存 class/methodID +
  按 argKinds marshalling + Call[Static]<Type>MethodA）+ `mj2x_vm_call_table_<tag>[]`
  分派表；wrapper 传该表给 exec（纯计算方法传 0）
- 验收（已达成）：4 种 MSVC 可编译 dispatcher（switch/funcptr/ifnest/state）下，
  drift 全开（2 变体 + 16 字节周期），`-Xverify:all` round-trip 绿：
  - `StaticCallChainRoundTrip`：7 方法（int/long/double 调用链 + methodRef 去重）
  - `InstanceCallOnObjectParamRoundTrip`：static 方法对对象参数做 invokevirtual

> 注：原计划 "getfield + invokevirtual" 中的 getfield 属 2.A（LoadField 未实现），
> spike 以 "REF + Call" 闭环先行验证对象/调用语义；invokevirtual 经 static 调用方
> 对对象参数的调用路径验证（full 实例方法 virtual dispatch 仍待 2.B，因 backend
> 当前整体跳过实例方法）。

### Phase 2.A — 对象 / 字段 / 数组 ✅

五 opcode 全部落地，沿用 Phase 2.0 的 per-site helper + 分派表模式（统一签名
`mj2x_vm_call_fn`，exec 按 family 加表参数，空表传 0）：

- **LoadField/StoreField**（getfield/putfield/getstatic/putstatic）：per-site field
  helper，懒缓存 owner class + fieldID，按 desc 首字符精确选 JNI accessor
  （GetBooleanField/GetByteField/.../GetObjectField——字段 accessor 必须匹配声明类型）
- **ArrayLoad/ArrayStore**（iaload..aaload / iastore..aastore）：per-site helper，按
  VmArrayElemKind（9 精确类型，区分 boolean[]/byte[]——同 baload 但 elementType 不同）
  选 Get/Set<Type>ArrayRegion(len=1) 或 Get/SetObjectArrayElement；null 数组显式抛 NPE
  （prelude 新增 mj2x_vm_throw_npe），越界交 JNI 抛 AIOOBE
- **Allocate**（NEW/NEWARRAY/ANEWARRAY）：Object = AllocObject（<init> 由独立 Call 调），
  Array = New<Type>Array / NewObjectArray；MULTIANEWARRAY 暂 fail-closed（递归分配复杂）
- **array.length** intrinsic：GetArrayLength + null NPE，单一 helper（siteIdx 恒 0）

验收（已达成）：4 dispatcher × drift 全开 `-Xverify:all` round-trip 绿：
- `FieldAccessRoundTrip`（12 方法：int/long/double/ref/boolean/byte/char/short 字段 + 静态）
- `ArrayAccessRoundTrip`（10 方法：各元素类型读写）
- `AllocateAndArrayLengthRoundTrip`（6 方法：对象分配+字段、int/double/ref 数组、array.length）
- `MixedNativeAndFailClosedRoundTrip` 的 useInst（new+<init>+invokevirtual）现转 VM

> handler 复用方案：未走 RegC `_mj2x_regc_*` helper（varargs + 全局 env 与 VMC4 寄存器
> 索引签名不兼容），改用 per-site 独立 helper（与 Call 一致）。VMC4 测试 66→78 全通过。

### Phase 2.B — 调用与虚分派 ✅

- 实现 opcode：Call（invokevirtual/special/static/interface 四种 dispatch）
- 复用 RegC class/method cache 机制（懒缓存 + 无锁 CAS）
- 验收：G6 方法调用样本 + 含虚分派/接口调用样本 round-trip 绿

**落地记录（2026-08-13，feat/mj2x-vmc4-calls）**：
- backend 不再跳过实例方法：wrapper 把 this 写 regs[0].ref（SSA 约定 this=参数 id0），
  显式参数从 regs[1] 开始——与 encoder 的 numInputs 对齐
- invokespecial → CallNonvirtual<Type>MethodA（私有方法/构造器不走虚分派，helper
  需要 _cls 定义类）；invokevirtual/interface 走 Call<Type>MethodA（JVM 侧虚分派）
- 新增 e2e：VirtualDispatchAndInterfaceRoundTrip / InvokeSpecialAndNullReceiverRoundTrip
  （null receiver → NPE）
- 验收：40 e2e 全绿

### Phase 2.C — 异常语义（工程难度最大） ✅

- 实现 opcode：Throw
- 突破"异常区域非空整体 fail-closed"：编码异常区域 + 解释器 try-catch 分派
- athrow + try-catch handler 匹配 + 未捕获传播 + finally
- 验收：G10 异常/monitor 样本 round-trip 绿

**落地记录（2026-08-13，feat/mj2x-vmc4-calls）**：
- 编码期把 RegFunction.exceptionRegions 静态展开：每 (region, 受保护块) 一个
  VmExceptionRegionEntry（handlerBlock/caughtTypeIdx/exceptionReg/blockIdx + 边拷贝），
  catch 类型去重到 catchTypes（0xFFFF = catch-all/finally）；blockExcRegions 按块索引
  给活跃条目（异常表顺序）
- 检查点模式：每种 dispatcher 循环底部统一做 ExceptionCheck → ExceptionOccurred →
  ExceptionClear → 按块线性扫描活跃条目 → mj2x_vm_exception_matches（IsInstanceOf）
  → 匹配：执行边拷贝 + 异常对象写 exceptionReg + pc=handler；未匹配：重新 Throw 挂起
  pending exception + status=1 传播（wrapper 返回默认值，Java 侧抛）
- 除零在 Binary handler 直接 mj2x_vm_throw_arith；athrow（null → NPE）走 mj2x_vm_throw
- 新增 e2e：ExceptionRoundTrip（div/throw/nested catch 匹配与传播）；prelude 新增
  mj2x_vm_throw / mj2x_vm_throw_arith / mj2x_vm_exception_matches
- 验收：44 e2e 全绿

### Phase 2.D — Intrinsic 子集 + 双后端对拍 ✅

- Intrinsic opcode 透传 RegIR `Intrinsic("...")`，复用 RegC intrinsic registry
  子集（Math / wrapper / String）
- 对拍：VMC4 与 RegC 对同一方法输出一致
- 验收：intrinsic 子集双后端对拍通过

**落地记录（2026-08-13，feat/mj2x-vmc4-calls）**：
- SSA 产生的 intrinsic（array.length / cmp.nan_greater|nan_less / instanceof:<desc> /
  monitor.enter|exit）按名称泛化分派：VmIntrinsicRef 表（按 name 去重），字节流
  [Intrinsic][siteIdx][recv=0][dest][argc][args...]，发射 per-intrinsic helper
  （懒缓存 class / 纯计算 / JNI accessor）+ intrinsic_table
- 与 RegC 的差异：RegC 在 emitter 内联 intrinsic（如 cmp 三值比较展开成 C 表达式），
  VMC4 统一走 per-intrinsic helper；Math/wrapper 方法两后端都是普通 Call（JNI）
- 新增 e2e：IntrinsicAndMonitorRoundTrip（array.length/cmp/instanceof/monitor）、
  MathIntrinsicDualBackendParity（VMC4 自包含 vs RegC 链接 CRT 的对拍）
- 验收：48 e2e 全绿（含 4 dispatcher × dual-backend parity）

### Phase 2.E — 可用化收口 ✅

- 真实 jar（JavaObfuscatorTest TEST.jar）业务方法 VMC4 转化率从当前 0%（全
  fail-closed）拉到可用水平
- 12 个 e2e 扩展到含 A/B/C/D 场景
- trace surface 更新（`describe()` 纳入新 family），交付 mjdbg 红队
- 验收：TEST.jar 业务方法 VMC4 转化率报告 + 全 e2e 绿

**落地记录（2026-08-13，feat/mj2x-vmc4-calls）**：
- 转化率报告基建：`vmc4_conversion_test` 跑 pipeline 后解析日志（Compiling/Skipping），
  按签名回填状态，报告写入 test/mj2x/vmc4_conversion_report.md。初版 25%（1/4，
  Main.main / StringKernel.mix 的 String LDC、ArrayKernel.multi 的 MULTIANEWARRAY
  fail-closed）
- **String LDC**：ldc_string Opaque（tag 携带 UTF-8 内容，ssa_builder.cpp:99）在
  resolveOperand 普通路径物质化为字符串常量槽（VmStringLiteral，按内容去重，槽落在
  计算寄存器之后），wrapper 在 exec 前用 NewStringUTF 预加载
- **MULTIANEWARRAY**：移除 encodeAllocate 的 fail-closed，维度寄存器全在 argIdx[]
  （argc=维度数）；发射共享递归 helper（mj2x_vm_alloc_multi_array_<tag>，按 JVM
  eager 语义逐层 NewObjectArray/New<Type>Array，对齐 RegC _mj2x_regc_alloc_multi_array）
- e2e 扩展到 14 用例 × 4 dispatcher（新增 StringLdcAndMultiArrayRoundTrip，9 检查点；
  String 连接 `+` 在 javac 9+ 是 invokedynamic（indy:0），不在本 Phase 范围）
- **转化率：25% → 100%（4/4 全部转 VM）**；VMC4 测试 45 单测 + 56 e2e 全绿

## 主线一：RegC 性能基线（P1，贯穿）

### Phase 1.1 — 基准套件扩展

- 新增对象/字段、调用/虚分派、数组、intrinsic 四类场景基准（当前 3 个全纯计算）
- 对比基线纳入 **JNIC**（issue #787 点名）：同方法集 native 化对比 RegC
- JDK 17 / 21 双矩阵
- 验收：每份基准附覆盖率审计表（强制）+ JVM/StackC/RegC/JNIC 四列数据

> **完成（2026-08-13，feat/mj2x-regc-perf）**：jvm-obf-tester 13-probe 基准脚本
> （`benchmark/jvm_obfuscator_test_benchmark.ps1`，参数化 + javap 覆盖率审计）入库；
> JDK17 总耗时 2.25x→1.16x（122ms vs 105ms，Control flow 0.87x 已反超）、JDK21 1.56x；
> 正式报告 `benchmark/results/2026-08-13-jvm-obf-tester-os30.md`（RegC native 74.7% vs
> os30 61.6%）。

### Phase 1.2 — 针对性优化

- 通用 JNI helper 命中率审计：统计 `_mj2x_regc_call_*` 派发占比，定位未命中
  intrinsic 的热点调用点 → 扩 intrinsic registry
- 新增 intrinsic：System.arraycopy 等
- 方法内联 pass（RegIR 层）—— 按 1.1 数据决策是否必要
- 验收：基线上量化加速比（before/after）

> **部分完成（2026-08-13）**：Integer/Long 位操作 intrinsic 补全（rotate/reverse/
> reverseBytes/bitCount/前导尾随零/最高最低位/signum，issue #778）；异常检查稀疏化
> （纯计算 intrinsic 后省略 ExceptionCheck，Arithmetic 热循环 66x→2x）；resolved
> dispatch 双槽缓存（交替 receiver 不再每轮 refresh，issue #780）。仍待办：System.
> arraycopy 等更宽 intrinsic、方法内联 pass（按 1.1 数据决策）。

## 验证命令

```bash
# VMC4 单测 + e2e
xmake build mj2x_unittest
xmake run mj2x_unittest --gtest_filter="Vmc4*:*Vmc4*"
# RegC 快测试组
xmake run mj2x_unittest --gtest_filter="RegCTest.*:RegCIntrinsicTest.*"
# 共享语义矩阵（mtest_set）
xmake build mj2x_integration_test
xmake run mj2x_integration_test
```

> 注意：`xmake f -P <子项目>` 会破坏 root 配置（issue #782），统一在脚本内每次 `f -c`。
> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`），未附覆盖率的数据不予采信。

## 下一阶段（v2.2 二期）：异常精确化 + 反侦测体系 + 文档整理

> 依据：PR #792 审查结论（issue comment 5282290947 的下一轮待办）+ mj2x `doc/roadmap.md`
> §3.4（"runtime 反侦测体系：从参数扫描/简单 debugger 检测，扩展到 agent/JVMTI/native
> debugger 场景"）。三条主线：

| 主线 | 内容 | 优先级 | 分支建议 |
|---|---|---|---|
| 三 | 异常语义精确化（稀疏异常：保护区对齐 JVM 指令级 `[start_pc, end_pc)`） | P1 | 独立分支（共享前端，风险高） |
| 四 | 反侦测体系（Linux antidebug + 反制响应层 + 后端接入 + agent 检测增强） | P2 | 一个分支承载（可含主线五文档） |
| 五 | 文档整理（capabilities/dev_plan/techniques/doc_ai 设计文档） | 贯穿 | 随主线四收口，或独立小提交 |

### 主线三：异常语义精确化（稀疏异常）—— P1

**审计结论（2026-08-13，推翻初版假设）**：javac 的异常表范围是整个 try 语句
（如 `try { if (x<0) throw ...; return x*2; }` 实测为 `[0, 15)`，不是仅 throw
块），且 code_converter 已把异常表 start/end/handler label 强制设为块入口
（analyzeControlFlow），因此 **javac 字节码下块粒度 == 指令粒度，不存在
实质性过度保护**（PR #792 审查时的初版分析基于错误的 `[8,16)` 假设）。

**已完成加固（feat/mj2x-exc-exact）**：保护区判定真源改为**指令序号区间**
（`ExceptionHandler::startInsnIdx/endInsnIdx` ∩ `Block::firstInsnIdx/lastInsnIdx`，
同一指令列表下标），取代依赖"块序 == 字节码序"的 label 区间扫描。收益：
- 判定与 L1 label 切分解耦——未来块合并/重排/手工字节码（label 与指令同块）
  下仍精确
- 未解析时保留旧 label 扫描语义作 fallback（宁可过度保护不漏保护）
- 新测试锁定语义：`SSABuilderJavaSamples.ExceptionPrecisionRanges`
  （protectedBlocks 集合 == {块指令范围与保护区相交}）+
  `Vmc4BackendE2E.ExceptionPrecisionRoundTrip`（semiCatch 除零进 catch /
  semiAfter try 后除零传播，4 dispatcher）

**后续（可做，记录不阻塞）**：
- VMC4 检查点/RegC 分发的**指令级匹配**（blockExcRegions 升级为块内指令区间表，
  pc → 块内指令序号 → 区间匹配），彻底消除"半覆盖块"残留（当前 javac 字节码
  下不存在半覆盖块，故优先级低；需要给 MIR/SSA/RegIR 指令带原始指令序号）
- 异常表多 region 覆盖同一块的匹配顺序测试（嵌套 try 已有覆盖，可扩展）

### 主线四：反侦测体系（antidebug / antiagent）—— P2

**已完成（feat/mj2x-antidebug-agent）**：

| 阶段 | 内容 | 状态 |
|---|---|---|
| 4.1 | Linux antidebug：TracerPid（/proc/self/status 解析谓词 `hasTracerPid`）、PTRACE_TRACEME 自检（EPERM+TracerPid 非零 = 被跟踪；EPERM+零 = seccomp 限制 → nativeProbeFailed）、maps rwx 段（`hasWritableExecutableMapping`）、单调时钟时间差（弱信号，不进聚合） | ✅（stub 头 + MSVC 交叉编译验证 Linux 分支语法） |
| 4.2 | 反制响应层 `detection/enforcement`：EnforcementAction（LogOnly/Delay/FakeData/Terminate）+ 多阶段检查点（step 确定性下限 + xorshift 随机提前命中）+ force 语义 + checkAndEnforce 组合；Terminate 返回 outcome 由集成层执行（可测） | ✅ |
| 4.3 | 后端接入：mj2x-crt 新增 `rt_protect/mj2x_rt_protect.{h,c}`（C99 自包含检查点，resources.manifest 内嵌）；RegC/VMC4 生成器方法入口注入（`codegen/runtime_protection` 共用片段，默认不注入零产物变化）；CLI `--runtime-protection <none\|log\|delay\|fake\|terminate>` + `--runtime-protection-step` + `--runtime-protection-delay`（RegC/VMC4 通用，其他 backend 报错）；真实 MSVC 编译链验证（VMC4/RegC 注入产物 + rt_protect.c → DLL） | ✅ |
| 4.4 | agent 检测增强：`collectProcessArguments`（Windows GetCommandLineW + 引号感知 `splitCommandLineArgs`；Linux /proc/self/cmdline）+ `collectLiveAgentSignals(javaVm)` 自动收集重载 | ✅ |
| 4.5 | 验证：runtime 单测（Linux 谓词 mock、enforcement 状态机、命令行切分）+ crt 检查点单测 + backend 注入单测 + CLI 校验单测 + 编译链 e2e | ✅ |

**后续（可做，记录不阻塞）**：
- agent 特征深度检测：javaagent jar 的 MANIFEST Agent-Class/Launcher-Agent-Class
  扫描（需类路径枚举，重）；JDWP 端口主动探测（易误报，命令行覆盖已足够）
- mj2x-runtime C++ 版与 crt C 版的检测面统一（当前独立实现，C 版为第一版
  子集：Windows 无 PEB/NtQuery 探测，Linux 无 ptrace 探测）
- 检查点注入位置扩展：热路径稀疏检查（循环内）而非仅方法入口；随机化注入位置
- 真调试器/agent 场景 E2E（Windows x64dbg/WinDbg attach、Linux gdb/ptrace +
  -javaagent 启动）→ mjdbg 攻击报告反馈

### 主线五：文档整理（贯穿）

- **capabilities.md 修正 VMC4 现状**（本次已同步 2.B-2.E：对象/字段/数组/调用/
  异常/intrinsic 全支持、45 单测 + 56 e2e、TEST.jar 转化率 100%、athrow 归属修复）
- doc/techniques/runtime_protection.md 补充 Linux 反调试/Java agent 检测通用技术
- doc_ai/ 新增设计文档：异常精确化设计（随主线三）、反侦测体系设计（随主线四）
- doc/roadmap.md 状态同步（VMC4 可用版收口；反侦测体系列入 v2.2）

### 主线六：剩余 issue 规划（2026-08-14 梳理）

> 依据：#787 §3 余项、#762 升级档。已完成的 issue 状态与 PR 对应见 issue 本体
> （#748→#754/#768、#762 低风险档→#766、#814→#816）。

| 待办 | 依据 | 优先级 | 前置条件 | 建议形态 |
|---|---|---|---|---|
| **方法内联 pass（RegIR 层）** | #787 §3.1 余项 | P1（性能） | 1.1 基线数据已就绪；直调默认关闭后 JNI 开销成为热点，内联收益放大 | 独立分支 feat/mj2x-regc-inline；验收 = benchmark 加速比 + 全量单测 |
| **System.arraycopy 等更宽 intrinsic** | #787 §3.1 / dev_plan 1.2 | P1（性能） | 无 | 并入内联分支或独立小提交 |
| **VMC4 升级档：period 语义变义** | #762 升级档 | P2 | 低风险档验证通过（已达成）；mjdbg 对抗验证闭环（#756 就绪） | 独立分支 feat/mj2x-vmc4-mutation；红队契约扩展同批 |
| **红队契约扩展（变义强度模型）** | #762 | P2 | mjdbg 就绪 | 随升级档 |
| **VMC4 检查点指令级异常匹配** | dev_plan 主线三"后续" | P3（记录不阻塞） | javac 字节码下块粒度==指令粒度，无实际缺口 | 需要 MIR/SSA/RegIR 指令带原始指令序号 |
| **agent 特征深度检测（javaagent MANIFEST 扫描 / JDWP 探测）** | 主线四"后续" | P3（记录不阻塞） | 类路径枚举；JDWP 易误报 | 随反侦测迭代 |

> 注：1.6 Pool（#814）与 2.8 Sec 为测试固有竞态/良性项，不做转换侧修复；
> 直调开关（#816）保持默认关闭，性能敏感产物显式 `--regc-direct-calls`。

## 日志

- 2026-08-14：**issue 梳理**（#748 关闭标 PR #754/#768；#762 低风险档=PR #766 勾选；
  #787 mj2x 段勾选 VMC4 可用版/性能线并标 PR；#783 记录现状评论）。文档系统梳理：
  readme.md 补 VMC4/模块/构建/索引；doc_ai/README.md 索引新建；roadmap.md 同步 v2.2
  现状（VMC4 主线完成、反侦测完成）；architecture.md 补 VMC4 后端；user_guide.md 补
  后端专属参数表与 --generate-xmake、修正 vmc4 状态；capabilities.md 新增 §4 VMC4
  章节（章节重编号）+ RegC 直调说明；mvp_release_scope.md 归档标注；regc_backend_readme.md
  状态更新。**#816 直调默认关闭已合入**（性能影响 ~1%）
- 2026-08-13：开分支 feat/mj2x-v2.2；依据 issue #787 §3 建立本计划（替换已完结的
  RegC 模板引擎旧计划）
- 2026-08-13：VMC4 发射器迁移到 codegen 工具层（CSourceStream），消除源码 `+=` 拼接
- 2026-08-13：spike 基础设施——REF 类型 + Call 编码/wrapper/exec 签名（不含 helper 发射）
- 2026-08-13：适配 libca 最新 API（startsWith/SHA1 getHash）+ 发射器改用 ca::str::format
- 2026-08-13：**Phase 2.0 spike 收尾完成**——per-callsite JNI helper + call_table 发射
  （`emitCallInfrastructure`），encodeCall 修正 instance 调用 receiver 取 args[0]
  （SSA 约定），VmMethodRef 加 argKinds/retKind 供 helper marshalling。新增 2 个 e2e
  （StaticCallChain / InstanceCallOnObjectParam），4 dispatcher × drift 全开 round-trip
  绿，VMC4 测试 58→66 全通过
- 2026-08-13：**Phase 2.A 完成**（feat/mj2x-vmc4-fields-arrays）——五 opcode（LoadField/
  StoreField/ArrayLoad/ArrayStore/Allocate）+ array.length intrinsic 全部落地，per-site
  helper 模式（exec 按 family 加表参数）。新增 3 个 e2e（FieldAccess/ArrayAccess/
  AllocateAndArrayLength），4 dispatcher × drift 全开 round-trip 绿，VMC4 测试 66→78 全通过。
  MULTIANEWARRAY 暂 fail-closed；instance field/array null NPE 已处理（field null 待补）
- 2026-08-13：**Phase 2.B 完成**（feat/mj2x-vmc4-calls）——backend 不再跳过实例方法
  （this → regs[0].ref），invokespecial → CallNonvirtual（_cls 定义类），wrapper 传
  intrinsic_table；新增 VirtualDispatchAndInterfaceRoundTrip /
  InvokeSpecialAndNullReceiverRoundTrip，40 e2e 全绿
- 2026-08-13：**Phase 2.C 完成**（feat/mj2x-vmc4-calls）——VmExceptionRegionEntry 编码
  （region×保护块展开 + catch 类型去重 + blockExcRegions 按块活跃条目）、检查点模式
  （每个 dispatcher 循环底部 ExceptionCheck → 按块线性扫描 → IsInstanceOf 匹配 →
  边拷贝 + 异常写 exceptionReg；未匹配重新 Throw + status=1 传播）、除零
  mj2x_vm_throw_arith；新增 ExceptionRoundTrip，44 e2e 全绿
- 2026-08-13：**Phase 2.D 完成**（feat/mj2x-vmc4-calls）——Intrinsic 按名称泛化分派
  （array.length/cmp.nan_*/instanceof:<desc>/monitor.enter|exit 进 intrinsics 表），
  Math/wrapper 走普通 Call；新增 IntrinsicAndMonitorRoundTrip +
  MathIntrinsicDualBackendParity（VMC4 vs RegC 对拍），48 e2e 全绿
- 2026-08-13：**Phase 2.E 完成**（feat/mj2x-vmc4-calls）——String LDC（ldc_string Opaque
  物质化为常量槽，wrapper NewStringUTF 预加载）+ MULTIANEWARRAY（共享递归 helper，
  按 JVM eager 语义）落地，转化率报告基建（test/mj2x/vmc4_conversion_report.md）；
  TEST.jar 转化率 25%→100%（4/4）；新增 StringLdcAndMultiArrayRoundTrip e2e；
  45 单测 + 56 e2e 全绿。VMC4 可用版主线（2.0-2.E）收口完成；RegC 性能线 1.1/1.2
  留待数据驱动迭代
- 2026-08-13：**RegC 性能主线（feat/mj2x-regc-perf）**——Phase 1.1 完成：jvm-obf-tester
  13-probe 基准脚本 + javap 覆盖率审计入库，JDK17 总耗时 2.25x→1.16x（122ms vs 105ms，
  Control flow 0.87x 反超）、JDK21 1.56x，正式报告 `benchmark/results/2026-08-13-jvm-obf-tester-os30.md`。
  Phase 1.2 部分完成：Integer/Long 位操作 intrinsic 补全（#778）、异常检查稀疏化
  （Arithmetic 66x→2x）、resolved dispatch 双槽缓存（#780）。回归：mj2x_unittest 442/442、
  mj2x_integration_test 36/36 全过
- 2026-08-13：**主线三 异常精确化（feat/mj2x-exc-exact，PR #796）**——审计实证 javac
  异常表覆盖整个 try（throwCatch 实测 [0,15) 而非初版假设 [8,16)），且 code_converter
  强制异常表 label 成块入口 → javac 字节码下块粒度==指令粒度（推翻 PR #792 审查的
  初版"过度保护"结论）；blockInProtectedRange 判定真源改为指令序号区间（handler
  start/end insn idx ∩ block first/last insn idx），与 L1 切分解耦；新增 SSA 保护区
  集合断言 + VMC4 semi 形态 e2e；184+36 回归全绿
- 2026-08-13：**主线四 反侦测体系（feat/mj2x-antidebug-agent，PR #797）**——4.1 Linux
  antidebug（TracerPid/PTRACE_TRACEME 自检/maps rwx/时间差弱信号）；4.2 enforcement
  反制响应层（4 action + 多阶段检查点）；4.3 mj2x-crt rt_protect C99 检查点 +
  RegC/VMC4 方法入口注入 + CLI --runtime-protection（默认不注入零产物变化，真实
  MSVC 编译链 → DLL 验证）；4.4 agent 参数收集（Windows 引号感知切分/Linux
  cmdline）；61+全量回归绿；真实样本转化率 TEST.jar 100% / snake(AquaShield) 73% /
  jvm-obf-tester 85%（fail-closed 均已知 MVP 边界，报告本地）

### 主线七：mj2x 自建测试集（issue #820，2026-08-14 记录，待做）

> 用户需求（2026-08-14）：mj2x 需要**自己的系统性测试集**，在 mtest_set 中新建测试，
> 吸收开源测试经验并超越——做得更全、更好。参考：
> huzpsb/JavaObfuscatorTest（13-probe，性能/语义，已吸收 os30 对比基线）、
> bludnov/Obfuscation-Tester（混淆器语义保持统一测试面）、ttvar/jvm-obf-tester
> （JVM 混淆器行为对比）。

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 需求调研：三家测试集用例清单/断言方法/评分口径对比 | 待做 |
| 2 | 设计：mtest_set 中 mj2x 测试集目录结构、用例模板、断言器、矩阵配置（后端 × JDK × 混淆形态） | 待做 |
| 3 | 首批用例：语义保持核心面（算术/控制流/异常/对象/数组/调用/反射/动态代理/多线程/String/集合/时间/IO） | 待做 |
| 4 | 三后端矩阵（StackC/RegC/VMC4 × drift/种子/粒度变体）同测试面 | 待做 |
| 5 | 性能回归自动化（benchmark 13-probe 扩展并入测试集） | 待做 |
| 6 | mjdbg 对抗闭环（每个用例 expected trace surface + weakness report） | 待做 |

验收：测试集覆盖面和断言强度超过三家参考；三后端/多 JDK 一键矩阵；性能与语义
同一入口回归。

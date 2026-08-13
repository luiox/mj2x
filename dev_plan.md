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
| 2.B | VMC4 | Call（virtual/special/static/interface + cache） | 待开始 |
| 2.C | VMC4 | Throw + 异常区域编码 | 待开始 |
| 2.D | VMC4 | Intrinsic 子集 + 双后端对拍 | 待开始 |
| 2.E | VMC4 | 可用化收口（真实 jar 转化率 + e2e 扩展 + trace surface） | 待开始 |
| 1.1 | RegC 性能 | 基准套件扩展（含 JNIC 对比 + JDK 17/21） | 待开始 |
| 1.2 | RegC 性能 | 针对性优化（intrinsic 扩面 / 通用 helper 收敛 / 内联评估） | 待开始 |

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

### Phase 2.B — 调用与虚分派

- 实现 opcode：Call（invokevirtual/special/static/interface 四种 dispatch）
- 复用 RegC class/method cache 机制（懒缓存 + 无锁 CAS）
- 验收：G6 方法调用样本 + 含虚分派/接口调用样本 round-trip 绿

### Phase 2.C — 异常语义（工程难度最大）

- 实现 opcode：Throw
- 突破"异常区域非空整体 fail-closed"：编码异常区域 + 解释器 try-catch 分派
- athrow + try-catch handler 匹配 + 未捕获传播 + finally
- 验收：G10 异常/monitor 样本 round-trip 绿

### Phase 2.D — Intrinsic 子集 + 双后端对拍

- Intrinsic opcode 透传 RegIR `Intrinsic("...")`，复用 RegC intrinsic registry
  子集（Math / wrapper / String）
- 对拍：VMC4 与 RegC 对同一方法输出一致
- 验收：intrinsic 子集双后端对拍通过

### Phase 2.E — 可用化收口

- 真实 jar（JavaObfuscatorTest TEST.jar）业务方法 VMC4 转化率从当前 0%（全
  fail-closed）拉到可用水平
- 12 个 e2e 扩展到含 A/B/C/D 场景
- trace surface 更新（`describe()` 纳入新 family），交付 mjdbg 红队
- 验收：TEST.jar 业务方法 VMC4 转化率报告 + 全 e2e 绿

## 主线一：RegC 性能基线（P1，贯穿）

### Phase 1.1 — 基准套件扩展

- 新增对象/字段、调用/虚分派、数组、intrinsic 四类场景基准（当前 3 个全纯计算）
- 对比基线纳入 **JNIC**（issue #787 点名）：同方法集 native 化对比 RegC
- JDK 17 / 21 双矩阵
- 验收：每份基准附覆盖率审计表（强制）+ JVM/StackC/RegC/JNIC 四列数据

### Phase 1.2 — 针对性优化

- 通用 JNI helper 命中率审计：统计 `_mj2x_regc_call_*` 派发占比，定位未命中
  intrinsic 的热点调用点 → 扩 intrinsic registry
- 新增 intrinsic：System.arraycopy 等
- 方法内联 pass（RegIR 层）—— 按 1.1 数据决策是否必要
- 验收：基线上量化加速比（before/after）

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

## 日志

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

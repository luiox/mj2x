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
| 2.0 | VMC4 | Spike：REF 类型 + 调用链最小闭环 + 决策 handler 复用方案 | 待开始 |
| 2.A | VMC4 | LoadField/StoreField/ArrayLoad/ArrayStore/Allocate | 待开始 |
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

### Phase 2.0 — Spike：对象模型 + 调用链最小闭环

**目标**：验证 REF 类型承载 jobject、handler 复用 CRT 共享机制、含 `getfield` +
`invokevirtual` 的真实方法端到端跑通。此 Phase 决策后续全部 VMC4 工作的落地形态。

- REF 类型建模：确认 `mj2x_vm_slot` 能承载 jobject（当前只放 i32/i64/f32/f64 标量）
- CRT 共享机制：RegC runtime prelude 抽出为 VMC4 解释器可 include 的片段
- 最小闭环：LoadField + Call 各一个 opcode 跑通 round-trip
- 验收：一个含 `getfield` + `invokevirtual` 的真实方法，4 种 MSVC 可编译 dispatcher
  下 `-Xverify:all` round-trip 绿

### Phase 2.A — 对象 / 字段 / 数组

- 实现 opcode：LoadField / StoreField / ArrayLoad / ArrayStore / Allocate（+ ArrayLength）
- handler 委托 RegC `_mj2x_regc_load/store_*_field` / `_mj2x_regc_alloc_*` /
  `_mj2x_regc_array_length`
- 编码器：字段/数组元素类型 tag、static/instance 区分、class/field cache CSE
- 验收：mtest_set G3（类型转换）/G6（方法调用里的字段）/G7（字段）/G9（对象与类型）
  样本在 VMC4 ≥2 种 dispatcher 下 round-trip 绿

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

# VMC4 CHECKCAST（ReferenceCast）支持设计评估

> 状态：设计笔记（未实现）。依据：真实样本转化率验证（2026-08-13）——
> snake.jar（AquaShield）fail-closed 7 个中 6 个是 `Convert: ReferenceCast not
> supported`，jvm-obf-tester.jar 22 个 fail-closed 中主要也是 ReferenceCast。
> checkcast 是真实 jar 转化率的第一瓶颈。

## 现状

- `vm_encoder.cpp:835`：Convert 只支持 Numeric 转换，ReferenceCast（CHECKCAST）
  直接 fail-closed（"MVP 只支持 Numeric convert"）。
- 但 **instanceof 已支持**：`instanceof:<desc>` intrinsic 走 IsInstanceOf helper
  （懒缓存 class，null → false），Phase 2.D 落地，4 dispatcher e2e 覆盖。

## 可行性：组件已齐

| 组件 | 状态 | 用途 |
|---|---|---|
| IsInstanceOf（懒缓存 class） | ✅ 已实现（instanceof intrinsic） | checkcast 类型检查 |
| `mj2x_vm_throw` | ✅ 已实现（Phase 2.C） | 失败时抛 ClassCastException |
| 异常检查点/保护区捕获 | ✅ 已实现（Phase 2.C） | checkcast 抛异常归属所在块，保护区内被 catch |
| per-site helper + 分派表 | ✅ 已实现（Call/field/array 同模式） | checkcast:<desc> 站点表 |

## 方案

CHECKCAST 编码为 per-desc 的 `castcheck:<desc>` 语义（镜像 `instanceof:<desc>`
intrinsic 的按名称泛化分派）：

1. **编码**：`encodeConvert` 的 ReferenceCast 分支不再 fail-closed——收集
   `CastCheckRef{desc}`，字节流存 siteIdx；intrinsic 表按 desc 去重（同类
   多个 checkcast 共享 helper）。
2. **helper**：`mj2x_vm_castcheck_<idx>(env, reg)` → `IsInstanceOf(desc)`：
   - null → 原样返回（JVM 语义：null 通过）
   - 命中 → 原样返回
   - 未命中 → `mj2x_vm_throw(env, ClassCastException)` + 返回 null
     （异常经检查点分派，归属 = checkcast 所在块）
3. **解释器**：`CastCheck` opcode：`regs[dst] = table[siteIdx](env, regs[src].ref)`；
   调用后按既有检查点模式（mayThrow 指令后 ExceptionCheck）处理异常传播。
4. **异常 message**：JVM 格式 `class X cannot be cast to class Y`（可选；
   空 message 也符合规范，第一版可空以省字符串常量槽）。

## 语义边界

- checkcast 是**纯类型检查**（不产生值变换），SSA/RegIR 的 Convert 语义已对齐。
- 异常归属：检查点块反查（athrow 归属修复后逻辑）天然覆盖——checkcast 指令
  在保护区内抛的 ClassCastException 会被对应 catch 捕获（如 `(String)x` 在
  try 内）。
- 与 instanceof 的差异：instanceof 返回 boolean 不抛；checkcast 抛。helper
  独立，class 缓存可与 instanceof 共享（同一 desc 复用 `mj2x_vm_class_cache`）。

## 预期影响

- snake.jar 转化率 73% → ~92%（7 个 fail-closed 中 6 个 ReferenceCast 可转；
  剩余 switch terminator 属另一项）。
- jvm-obf-tester.jar 85% → ~95%。
- 体积：每 desc 一个 helper（按 name 去重），与 instanceof 同量级。

## 测试计划（实施时）

- 单测：编码（CastCheck site 表 + 去重）、null 通过、失败抛 ClassCastException
- e2e：`checkcast 成功 / 失败 / null / try 内失败被 catch` 4 dispatcher × drift
  全开（Vmc4BackendE2E）
- 样本回归：snake.jar / jvm-obf-tester.jar 转化率复测

## 后续项（不在本笔记范围）

- switch terminator（tableswitch/lookupswitch）支持评估——转化率第二瓶颈
- NullLiteral operand、Opaque ldc_class 的宽松化评估

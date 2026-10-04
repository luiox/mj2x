# intrinsic / fast-path × 后端形态覆盖矩阵（#1117）

> 状态：#1117 首版（2026-10-02）。本文是「intrinsic/fast-path × VM 形态」覆盖对照表，
> 动因是 jvmtp 1.7.1 的现成反例：其 `intrinsics.hotspot.field` / `.refs` fast-path
> 只覆盖 stack 机、register 机漏配，两机混布时丢 direct-memory 字段访问——多形态 VM
> 的能力矩阵不对齐就会出同类坑。mj2x 的「多形态」是 StackC / RegC / LLVM / VMC4 四后端，
> 本文逐面标注各后端现状（fast-path / 走 JNI / 不适用），并给出高价值判据与候选行。
> 调研 ledger：morpher#760 issuecomment-5969219464（jvmtp 1.7.1 逐条判读）。

## 1. 高价值判据

一面（face）值得补 fast-path / 入 kEntries，须同时满足：

1. **高频调用面**：出现在分派、hash 查找、数值比较、装箱、字符串构建等热路径；
2. **现状走通用 JNI 多管道**：通用 `_mj2x_regc_call_*` / per-callsite helper
   每调用点 2-6 个 JNI，而 fast-path 可压到 0-1 个；
3. **语义可零 JVM 内部依赖 native 化**：纯 CRT 计算（无异常面或异常面已有
   通道），或不逃逸 shadow state（wrapper/Random/builder 模式）。

不满足判据 3 的面**只进对照表标候选，不进 kEntries**（#1027 B2 纪律：权威表
不收语义无把握的行；发射端 RuntimeFunction 省略 ExceptionCheck，任何可能抛
异常的纯计算行必须先有 precheck 位方案）。

## 2. 字节码级 intrinsic 面 × 四后端

状态口径：**fast-path** = 有专用发射形态（非通用 JNI 调用管道）；**JNI** = 经
JNI 底座（缓存 ID/helper 属 JNI 快路径，仍计 JNI）；**不适用** = 设计约束排除。

| 面 | StackC | RegC | LLVM | VMC4 |
|---|---|---|---|---|
| `arraylength` | JNI `GetArrayLength` | JNI `GetArrayLength` | JNI `GetArrayLength` | **intrinsic** `array.length`（per-intrinsic helper 泛化分派） |
| 浮点/long 三值比较（FCMPG/FCMPL/DCMPG/DCMPL/LCMP） | 纯 C 内联比较 | **intrinsic** `cmp.nan_*`（纯计算，省 ExceptionCheck） | **intrinsic** `cmp.nan_greater/nan_less` | **intrinsic** `cmp.nan_greater/nan_less`（纯计算无 JNI） |
| `instanceof` | JNI `IsInstanceOf` | JNI `IsInstanceOf`（TU 级 class cache） | JNI `IsInstanceOf` | **intrinsic** `instanceof:<desc>`（懒缓存 class） |
| `monitorenter/exit` | JNI `MonitorEnter/Exit` | JNI `MonitorEnter/Exit` | JNI `MonitorEnter/Exit` | **intrinsic** `monitor.enter/exit` |
| `checkcast` | JNI（类型检查 + 抛 CCE） | JNI `IsInstanceOf` + 抛 CCE | JNI `IsInstanceOf` + 抛 CCE | **fail-closed**（ReferenceCast 保留字节码；设计笔记 `vmc4_checkcast_support.md`，转化率第一瓶颈待解） |
| 字段读写（jvmtp `hotspot.field` 对应面） | 缓存 jfieldID + `Get/Set<Field>`（无锁 CAS 缓存） | 缓存 jfieldID + `Get/Set<Field>`（TU 级 CSE） | JNI `Get/Set<Field>` | per-callsite helper（懒缓存 field ID + 分派表） |
| ├ direct-memory 字段访问（jvmtp register 机新档形态） | **不适用（设计约束）** | **不适用（设计约束）** | **不适用（设计约束）** | **不适用（设计约束）** |
| `aastore` 协变类型检查（refs 面） | `SetObjectArrayElement`（JVM 原生 ASE 检查） | 同左 | 同左 | 同左 |
| String LDC 物化 | UTF-16 池 `NewString` + **intern 归一化 + TU 级 global-ref 缓存（#1117 修）** | `_mj2x_intrinsic_string_literal`（TU 缓存 + **intern 归一化（#1117 修）**） | `NewStringUTF` 逐次物化，**未 intern（缺口，候选）** | 常量槽 `NewStringUTF` 预载，**未 intern（缺口，候选）** |

说明：

- **direct-memory 字段访问标「不适用」而非缺口**：jvmtp 该档跳过 JNI 直接按
  JVM 内部布局读写对象内存，收益高但依赖 HotSpot 字段布局 / compressed oops /
  GC 屏障等内部 ABI。mj2x 全线设计约束是「对象模型全走 JNI、零 JVM 内部依赖」
  （capabilities §3 LLVM 条目明文），跟进即引入 per-JVM 版本/选项敏感性，且与
  三平台 safe-env 承诺冲突。除非产品层面推翻该约束，否则此面永久不适用。
- **aastore**：四后端都借 `SetObjectArrayElement` 让 JVM 自己做协变检查，
  正确性由 JVM 保证，无需自有 fast-path；`g17.array_store_check` 回归锁。
- **String LDC intern**：jvmtp 修复④面。#1117 修复覆盖 RegC（literal intrinsic
  内 intern + 缓存）与 StackC（池条目 intern + `_mj2x_literal_cache[]`）；LLVM /
  VMC4 的 ldc 物化仍是裸 `NewStringUTF`，同内容常量跨站点/与 `String.intern()`
  比较会失配——**已知缺口**，两后端列在白名单外（`g17.string_intern` 不进
  LLVM 列），修复方式同源（物化后 intern），待单卡。

## 3. JDK 方法 intrinsic 面（kEntries 权威表）× 后端

权威表 `mj2x/mj2x-core/src/mj2x/backend/regc/regc_intrinsic_registry.cpp` kEntries
（#1027 B2 单源，179 行）；LLVM 侧 `llvm_intrinsic_registry` 由
`builtinIntrinsicTable()` 的 `llvmDirect` 位机械派生，计数锁定在
`LlvmIntrinsicRegistryGoldenCross`。VMC4 / StackC 无 JDK 方法 intrinsic 机制，
下列面一律走普通 JNI 调用（`MathIntrinsicDualBackendParity` 等 E2E 对拍正确性）。

| 面（kEntries 行数） | RegC | LLVM | VMC4 / StackC |
|---|---|---|---|
| Math 标量 27 行：abs×4 / min-max I,J,F,D / signum F,D（#1117）/ exp,log,log10（#1117）/ sin,cos,tan,sqrt,floor,ceil,pow / round D,J + **round F,I（#1117）**（**#1144 起为 OpenJDK 17 位运算口径，floor(x+0.5) 公式废弃**）/ random | RuntimeFunction 直调 CRT（0 JNI、无 ExceptionCheck） | 直调同 CRT 符号（llvmDirect=true，declare 派生） | 普通 JNI 调用 |
| Integer/Long 位操作 24 行（rotate/reverse/bitCount/nlz/ntz/oneBit/signum/toHex/Octal/Binary） | RuntimeFunction | JNI 回退（llvmDirect=false，行为扩展须随 LLVM 卡验证） | 普通 JNI 调用 |
| parse/toString/compare 34 行（Integer/Long/Short/Byte/Float/Double/Boolean/Character + String.valueOf×7） | RegCLocalHelper（context 回调路由异常） | JNI 回退（llvmDirect=false） | 普通 JNI 调用 |
| wrapper value/unbox 16 行 | WrapperValueOf/Unbox（不逃逸 shadow struct） | 同构 shadow（llvmDirect=true） | 普通 JNI 调用 |
| Random 7 行（init(J) + next* 6 面，nextInt(bound) 带 PositiveBound precheck） | NativeObjectMethod shadow state | 同构状态机（llvmDirect=true，precheck 透传） | 普通 JNI 调用 |
| StringBuilder/StringBuffer 49 行 | opaque UTF-16 CRT state + escape 回退 | 同构状态机（llvmDirect=true，trimToSize 除外） | 普通 JNI 调用 |
| String 方法 14 行（length/isEmpty/startsWith/endsWith/concat/compareTo/indexOf×4/replace/trim/split×2/contains） | StringMethod/StringRuntimeMethod | JNI 回退（llvmDirect=false） | 普通 JNI 调用 |
| Enum name/ordinal、System.arraycopy、Object.getClass | RuntimeFunction / RegCLocalHelper / ObjectGetClass | JNI 回退 | 普通 JNI 调用（Enum 合成面 values/valueOf 走后端预扫描内联，不入表） |

## 4. 候选行（明确未入表 + 裁决理由）

| 候选 | 现状 | 不入表理由 / 前置条件 |
|---|---|---|
| `Math.floorDiv/floorMod` (II)I (JJ)J | 走 JNI | 纯计算但除零须抛 `ArithmeticException`；RuntimeFunction 发射省略 ExceptionCheck，须先扩 precheck 位（`NonZeroDivisor`，#879 机制可容纳）并双发射端（RegC/LLVM）接线。收益中等，待卡。 |
| `Integer/Long.sum/max/min` | 走 JNI | 纯计算零异常面，CRT 三行即成；收益中低（streams 场景），搭 floorDiv 同车。 |
| LLVM/VMC4 String LDC intern | 裸 NewString 物化 | 修复方式同 #1117 RegC/StackC（物化后 intern）；LLVM 列入 emitter、VMC4 列入常量槽预载——VMC4 发射面与 #1116 并行卡冲突，待其合入后单卡。 |
| `String.equals/charAt/hashCode/substring` | **有意排除（文档化判据）** | 多 JNI 数据管道实测劣于 1-JNI 缓存调用；HotSpot 对前三者有 VM intrinsic + hashCode memoization。勿翻案（capabilities §3 同款判据）。 |
| `StrictMath.*` | 走 JNI | **不适用**：严格语义必须走真 StrictMath，native 直调 CRT 无严格档保证。 |
| `Object.clone` / `System.identityHashCode` / `Thread.onSpinWait` 等 | 走 JNI | 单次 JNI 调用即最优形态，fast-path 无收益空间。 |
| CONSTANT_Float/Double NaN 载荷位型不保（**StackC 限定**，NAN 宏规范化） | StackC LDC 物化走 `NAN`/`(double)NAN` 宏（`stackc_mir_arith.cpp`），载荷位型与符号位被规范化为标准 quiet NaN | `Float.floatToRawIntBits`/`Double.doubleToRawLongBits` 透传可观测发散。RegC（`_mj2x_regc_f32/f64_from_bits` 位型直写）与 LLVM（hex 字面量，F32 经 double 加宽）已保真。候选修法：StackC NaN 常量改位型物化（对齐 RegC 形态）。待卡。 |

## 5. #1117 补位记录

- **kEntries +6 行**（llvmDirect=true 全开，LLVM 派生随行）：`Math.round(F)I`、
  `Math.signum(F)F`、`Math.signum(D)D`、`Math.exp(D)D`、`Math.log(D)D`、
  `Math.log10(D)D`；CRT 符号 `mj2x_math_round_f32 / signum_f32 / signum_f64 /
  exp / log / log10` 落 `mj2x-rt/src/regc/mj2x_regc_intrinsic.{h,c}`。
  173 → 179 行，LLVM 直调面 92 → 98。
- **正确性修复（回归样本暴露）**：
  1. `mj2x_math_min/max_f32/f64` 的 ±0.0 结果符号（裸三目返回错误符号零，
     jvmtp 1.7.1 修复③同款缺陷）——OpenJDK max/min 语义（NaN 返回操作数 +
     ±0.0 按符号位裁决），`g17.math_nan_zero` 对拍锁定。
  2. LDC 字符串 intern 恒等（jvmtp 修复④同款缺陷面）：RegC literal intrinsic
     与 StackC 池物化经 `String.intern()` 归一化，同内容常量跨站点/跨方法/与
     `.intern()` 比较恒等；`g17.string_intern` 对拍锁定。
  3. StackC `if_acmpeq/acmpne` 裸句柄指针比较（g17.string_intern 联合定位）：
     JNI 引用是句柄，同一对象的 local ref 与 global ref 指针值不同——ldc 缓存
     （global ref）与 `String.intern()` 返回值（local ref）恒不等。改走
     `IsSameObject`（RegC 的 Compare 已是同口径，见 `regc_flow.cpp` 注释）。
  4. StackC 浮点 LDC 字面量补形：`formatFloatLiteral`（`%.*g`）对整数值浮点
     输出 "1"/"-0"，拼 `f` 后缀是非法 C（生成物 MSVC 编译失败），"-0" 转
     double 还丢符号位；新增 `codegen::cFloatLiteral` 统一补 ".0"（LDC 与
     indy/装箱实参两处调用点）。`g17.math_nan_zero` 的 ±0.0 行锁定。
- **样本侧防复发坑**（`wave4_samples.hpp` 注释在案）：`ANEWARRAY` 前必须推
  count；`String.intern()` 是实例方法（INVOKEVIRTUAL）；`@Deprecated` 需
  `ACC_DEPRECATED` 属性 + RVA 条目（`Ljava/lang/Deprecated;`，java.lang 包）
  双写——HotSpot 对解析失败的注解条目静默忽略。
- **回归样本 G17 五条**（mtest_set Wave4，跨界声明见提交）：`g17.math_nan_zero`
  / `g17.string_intern` / `g17.array_store_check` / `g17.sibling_methods` /
  `g17.annotation_visibility`——对应 jvmtp 1.7.1 五项正确性修复语义点，
  StackC/RegC 列矩阵全覆盖 + LLVM 列白名单（intern 例外），JVM 基线逐语义点判等。
- **#1144 追记（2026-10-02）**：`mj2x_math_round(F)I/(D)J` 由 floor(x+0.5)
  公式改 **OpenJDK 17 位运算口径**（JLS tie→+∞ + 位路径 + Java-cast 饱和），
  F/D 同修——公式版在 0.49999997f（给 1，正确 0）与 [2^23,2^24) 奇整数
  （8388609.0f 给 8388610，正确 8388609）可观测发散（JDK-8010430 类缺陷）。
  对拍：rt 单测 F 27 项/D 22 项边界全表（HotSpot 17.0.20.1 实测硬编码）+
  移植算法与 `Math.round` 等价扫掠——float 侧 2^32 全位型穷举
  （4,294,967,296 点，评审方独立复现分块校验和全等）+ double 侧结构化
  边界带/随机采样（评审方 25,024,406 点 0 失配）。口径注记（#1186）：
  #1167 提交消息所写「约 1.4 亿点」系 double 侧扫掠计数误记，commit 已合并
  不可改，以本节精确口径为准。另 `g17.math_nan_zero` 补 5 个 round 边界
  方法进样本字节码面（四列含 JVM 基线对拍）。同卡接线补缺：emitDeclarations
  补 #1117 六符号前置声明（此前 RegC 发射面调用即缺声明）。
  #1186 起全量 float 穷举升格为常驻回归：`mj2x_rt_test` 追加 DISABLED_
  前缀测试（2^32 位型 vs 语义参考实现逐点对拍 + 64 块 sum/xor 校验和
  断言；日常套件不计入，`--gtest_also_run_disabled_tests` 触发）。

## 6. 维护纪律

- 新增 JDK 方法 intrinsic 只改 kEntries 一处（#1027 B2 单源）：`emitKind` /
  `nativeObjectKind` / `passContext` / `argCount` / `isStatic` / `llvmDirect` /
  `precheck` 逐行裁决；不确行的宁可不加，进 §4 候选。
- 新形态后端（register 化 VMC4 等）合入前先对 §2 表逐行补列——本文即为
  jvmtp 1.7.1「register 机漏配 intrinsic」事故的防复发清单。

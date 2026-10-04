# RegC JDK Intrinsic

> 设计文档:RegC 在码生成阶段把 JDK 常用方法调用识别为 intrinsic,直接调用 runtime ABI 或 TU-local helper 而非走通用 JNI 派发。可分发 C ABI 权威头为 `mj2x-rt/src/regc/mj2x_regc_crt.h`;intrinsic 实现统一在 `mj2x-rt/src/regc/mj2x_regc_intrinsic.c`(原 mj2x-runtime in-tree 版已退役)。

## 1. 目标

RegC 生成的 C 默认把方法调用翻译为 `_mj2x_regc_call_*`(走 JNI `Call*MethodA`)。
JNI 派发点是 c2j 的攻击锚点之一。intrinsic 把 JDK 常用方法从 JNI 路径移到 runtime
直调:生成 C 里这些调用点没有 `_mj2x_regc_call_*`,只有 runtime 符号调用。

仅边界保留 JNI:
- 入口 JNI wrapper(`Java_*`)
- String intrinsic 内部的 `NewStringUTF` / `GetStringUTFChars`(因 String 字面量必须
  经 JNI 物化为 jstring)

## 2. 架构分层

```
RegC emitter (src/mj2x/backend/regc/regc_emitter.cpp)
  └─ Call 分支 → RegCIntrinsicRegistry(owner/name/desc/opcode)
       ├─ Math.*      → mj2x_math_*      (纯计算)
        ├─ parseInt    → _mj2x_intrinsic_parse_int (runtime,需 JNI 取 String)
       ├─ Wrapper     → mj2x_* value struct shadow state(仅非逃逸 value/unbox)
       ├─ Random      → mj2x_random shadow state(仅非逃逸 new/init/next*)
       └─ String      → mj2x-rt UTF-16 ABI / mutable opaque state
  未命中 → _mj2x_regc_call_* (原 JNI 路径)

distributable CRT (mj2x/mj2x-rt/src/regc/)
  ├─ mj2x_regc_runtime.{h,c}      RegC 运行时主体(#894 下沉):call/load/store
  │                               helper、角色单例缓存、TLS env/ctx/owner_class、
  │                               trace 状态与 intrinsic 实现(string_literal/
  │                               parse_* 等);生成代码 extern 直访
  ├─ mj2x_regc_intrinsic.c        wrapper/Math/Random/位操作 intrinsic 权威实现
  │                               (原 mj2x-runtime in-tree C++ 版已删除,R3 整合)
  ├─ mj2x_regc_string.c           String UTF-16 operations + split fast path
  └─ mj2x_regc_string_builder.c   StringBuilder/StringBuffer opaque state
```

**为什么全部在 mj2x-rt**:Math 是纯 C math.h;String intrinsic 与
`_mj2x_regc_call_*` 需要 `JNIEnv*`,物理下沉在 `mj2x_regc_runtime.c`(#894)。
单一可分发源(mj2x-rt)同时服务生成工程与 e2e 编译链,无双轨实现。

## 3. 已覆盖的 JDK 方法(初版)

### Math(`java.lang.Math`,static,无对象逃逸)
- `abs(I/J/F/D)` → `mj2x_math_abs_i32/i64/f32/f64`
- `min/max(II/JJ/FF/DD)` → `mj2x_math_min/max_*`(JLS NaN 传播,非 ISO C fmin/fmax;
  F/D ±0.0 结果符号按 OpenJDK 语义按符号位裁决 #1117——裸三目对 IEEE 相等的
  ±0.0 会返回错误符号零,g17.math_nan_zero 对拍锁定)
- `sin/cos/tan(D)D` → `mj2x_math_*`
- `sqrt(D)D`、`pow(DD)D` → `mj2x_math_*`
- `exp/log/log10(D)D` → `mj2x_math_*`(libm 半纯实现,#1117)
- `floor/ceil(D)D` → `mj2x_math_*`
- `signum(F)F/(D)D` → `mj2x_math_signum_f32/f64`(NaN/±0.0 按位型原样返回,#1117)
- `round(D)J` → `mj2x_math_round`（OpenJDK 17 位运算口径 #1144：JLS tie→+∞
  + 位路径 + Java-cast 饱和；floor(x+0.5) 公式已废弃——0.49999999999999994
  与 [2^52,2^53) 奇整数可观测发散，见 intrinsic_matrix.md §3/§5）
- `round(F)I` → `mj2x_math_round_f32`（OpenJDK 17 位运算口径 #1144 同上；
  floorf(x+0.5f) 公式已废弃——0.49999997f 给 1 正确 0）
- `random()D` → `mj2x_math_random`(Java 兼容 LCG 流)

### String / Integer
- `Integer.parseInt(Ljava/lang/String;)I` → `_mj2x_intrinsic_parse_int`
- String LDC → `_mj2x_intrinsic_string_literal`(TU 级缓存 + `String.intern()`
  归一化 #1117:ldc 常量是 interned 实例,同内容常量跨站点/跨类/与 `.intern()`
  比较恒等;缓存 32 槽溢出后逐次物化也因 intern 恒等不再破坏语义)
- String read hot paths、concat/substring/compareTo/indexOf/replace/trim 通过
  TU-local helper 或 `mj2x-rt` UTF-16 ABI 实现。
- `String.split(String[,limit])` 对简单字面量走 CRT fast path;空模式、转义和完整
  正则回退 JDK `String.split`,不在 CRT 自造正则引擎。
- StringBuilder/StringBuffer 使用 escape-aware opaque UTF-16 state;range/capacity 常用
  API 直接调用 CRT,逃逸时 materialize,逃逸后继续修改则从创建点回退 Java。

### Integer / Long 位操作(issue #778,#787,纯计算无 JNI)
- `Integer/Long.rotateLeft/rotateRight` → `mj2x_intrinsic_{int,long}_rotate_left/right`
- `Integer/Long.reverse`、`reverseBytes` → `mj2x_intrinsic_{int,long}_reverse(_bytes)`
- `Integer/Long.bitCount`、`numberOfLeadingZeros`、`numberOfTrailingZeros`、
  `highestOneBit`、`lowestOneBit`、`signum` → `mj2x_intrinsic_{int,long}_*`
- 语义与 OpenJDK 一致(distance 按 32/64 取模、无符号旋转/反转、符号位仅扩展在
  返回值),CRT 实现与 JDK 交叉验证(`mj2x-rt/test/mj2x_regc_intrinsic_test.c`)。
- RuntimeFunction emitKind 是**纯 C 计算**——emitter 对其省略 exception dispatch
  (见 `regc_codegen.md` 异常检查稀疏化),热循环每轮省一次 `ExceptionCheck` JNI。

### Primitive wrappers(第一切片,非逃逸)
- `Integer.valueOf(I)` + `Integer.intValue()` → `mj2x_int_value_of` / `mj2x_int_int_value`
- `Long/Short/Byte/Float/Double/Boolean/Character.valueOf` + 对应 `xxxValue`
  → `mj2x_*_value_of` / `mj2x_*_*_value`

RegC emitter 为 `valueOf` 结果维护 shadow native state:

```
r1 = Integer.valueOf(x)  => mj2x_int native_r1 = mj2x_int_value_of(x); r1 = NULL;
r2 = r1.intValue()       => r2 = mj2x_int_int_value(native_r1);
```

这个 shadow state 不是 jobject,因此第一切片只允许同一 RegC 函数内的
`valueOf -> xxxValue` 窄链路。若 shadow object 被返回、传入未知 call、存字段、
放数组、参与 monitor/checkcast/instanceof 或跨 CFG edge,当前策略是 fail-closed 并报告
`native object escaped through ...`。后续若要支持逃逸,需要在 valueOf 决策前做
use/escape 分析:可证明不逃逸才 native 化,否则从一开始 fallback 到 `_mj2x_regc_call_*`;
若对象已被 native unbox/mutation 后才逃逸,则必须提供 jobject materialize/writeback 桥。

### Random(第一切片,非逃逸)
- `new java.util.Random` → `mj2x_random native_rN = {0}`
- `Random.<init>(J)V` → `mj2x_random_init(&native_rN, seed)`
- `nextInt()` / `nextInt(bound)` / `nextLong()` / `nextBoolean()` /
  `nextFloat()` / `nextDouble()` → `mj2x_random_*(&native_rN, ...)`

Random state 是 mutable shadow native object,不会创建 jobject,所以 protected 方法内部
的 PRNG 序列可以完全不走 JNI。逃逸规则与 wrapper 相同:作为返回值、字段/数组存储、
未知调用参数或 CFG edge copy 时 fail-closed。后续 fallback 方案应在 `new Random`
时先做 use/escape 分析:若对象会逃逸,从分配开始就走 `_mj2x_regc_alloc_object` +
`_mj2x_regc_call_void("<init>")`;若已执行 native `next*` 后再逃逸,需要 jobject
materialize 并把 48-bit seed 写回 Java 对象,否则不能保持 Java 语义。

## 4. 未覆盖(后续扩展,见 dev_plan)

下列 runtime 实现已交付但**未完整接 emitter**,因涉及对象状态逃逸表示:

- **完整包装类对象语义**:identity、equals/hashCode/toString、返回 jobject、字段/数组存取
  仍未覆盖。阻塞点:escape-aware fallback 或 jobject materialize。
- **完整 Random 对象语义**:构造后返回、字段/数组存取、传给未知 Java 方法、跨 CFG edge
  仍未覆盖。阻塞点:escape-aware fallback 或 jobject materialize + seed writeback。
- **未登记的 String/mutable String 方法**:正常走 Java fallback。当前 registry 是常用
  签名集合,不是对 JDK 全量 API 的替代;正则 split 始终由 JDK 保持语义。

未登记的 JDK 方法正常走 `_mj2x_regc_call_*`,不会误识别(registry 条目与匹配断言见
`regc_intrinsic_registry_test.cpp`)。

## 5. 验证

- **runtime 单测**:`mj2x_runtime_test` 覆盖 in-tree intrinsic;`mj2x_crt_test` 覆盖
  可分发 C ABI、StringBuilder capacity/range 基础语义和 StringBuffer 并发 append。
- **emitter 单测**:`regc_intrinsic_test.cpp` 覆盖 registry、opaque state、cleanup、
  materialize/fallback 和无通用 JNI 派发断言。
- **E2E**:`RegCBackendE2E.IntrinsicMathAndParseIntRoundTrip` 在 JDK/MSVC 可用时真编 DLL,
  对比 StringBuilder/StringBuffer range/capacity、逃逸和 split literal/regex/异常语义。
- **mjdbg 样本**:`test_resource/protected_samples/regc/RegCIntrinsicSurfaceSample.java`,
  trace surface 见同目录 README。

## 6. 设计取舍

- **intrinsic 识别放 emitter(非 lowering/IR 节点)**:最小侵入,不动 SSA/RegIR 结构,
  符合"垃圾版起步"。后续若需在 IR 层做常量传播(如 `Math.abs(5)` 编译期求值),再迁到
  lowering/L4 pass。
- **Random 用 Java 兼容 LCG(非 XorShift128+)**:保证 E2E 与 `java.util.Random` 逐值
  可对比。XorShift128+ 留作未来高强度 PRNG(其他混淆用途)。
- **String 内容经 SSA opaque tag 贯通**:不新增 SSA value kind,复用 `SSAOpaqueValue`
  的 `tag` 字段(`"ldc_string:" + utf8`)。emitter 渲染时解析前缀。

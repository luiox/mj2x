# RegC JDK Intrinsic

> 设计文档:RegC 在码生成阶段把 JDK 常用方法调用识别为 intrinsic,直接内联 runtime 实现而非走 JNI 派发。权威 ABI 在 `runtime/src/intrinsic/*.hpp` 头文件 Doxygen。

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
       ├─ Math.*      → mj2x_math_*      (runtime lib,纯计算)
       ├─ parseInt    → _mj2x_intrinsic_parse_int (emitter TU 内联,需 JNI 取 String)
       ├─ Wrapper     → mj2x_* value struct shadow state(仅非逃逸 value/unbox)
       └─ Random      → mj2x_random shadow state(仅非逃逸 new/init/next*)
  未命中 → _mj2x_regc_call_* (原 JNI 路径)

runtime (mj2x/runtime/src/intrinsic/)
  ├─ intrinsic_wrapper.{hpp,cpp}  包装类 struct + valueOf/xxxValue
  ├─ intrinsic_math.{hpp,cpp}     Math 纯计算,转发 C math.h
  └─ intrinsic_random.{hpp,cpp}   Java 兼容 LCG

emitter TU (regc_backend.cpp::emitRuntimeHelpers)
  ├─ _mj2x_intrinsic_string_literal  NewStringUTF 物化 String LDC
  └─ _mj2x_intrinsic_parse_int       GetStringUTFChars + strtol + throw 回调
```

**为什么 Math 在 runtime、String 相关在 emitter TU**:runtime lib(`mj2x-runtime-core`)
不依赖 JDK include(Math 是纯 C math.h,String intrinsic 需要 `JNIEnv*` 的
`NewStringUTF`/`GetStringUTFChars`)。这与现有 `_mj2x_regc_call_*`(也在 emitter TU,
因需 JNI)一致。

## 3. 已覆盖的 JDK 方法(初版)

### Math(`java.lang.Math`,static,无对象逃逸)
- `abs(I/J/F/D)` → `mj2x_math_abs_i32/i64/f32/f64`
- `min/max(II/JJ/FF/DD)` → `mj2x_math_min/max_*`(JLS NaN 传播,非 ISO C fmin/fmax)
- `sin/cos/tan(D)D` → `mj2x_math_*`
- `sqrt(D)D`、`pow(DD)D` → `mj2x_math_*`
- `floor/ceil(D)D` → `mj2x_math_*`
- `round(D)J` → `mj2x_math_round`(floor(x+0.5),NaN→0,±Inf 钳位)
- `random()D` → `mj2x_math_random`(Java 兼容 LCG 流)

### String / Integer
- `Integer.parseInt(Ljava/lang/String;)I` → `_mj2x_intrinsic_parse_int`
- String LDC → `_mj2x_intrinsic_string_literal`(供 parseInt/未来 toString 消费)

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

`Integer.signum` 等未覆盖方法正常走 `_mj2x_regc_call_*`,不会误识别。

## 5. 验证

- **runtime 单测**:`mj2x_runtime_test` 的 `IntrinsicWrapperTest`/`IntrinsicMathTest`/
  `IntrinsicRandomTest`(22 用例,Random 逐位对齐 JDK)。
- **emitter 单测**:`regc_intrinsic_test.cpp`(Math/parseInt/wrapper value-unbox + 逃逸 reason,
  断言 intrinsic 被识别 + 无 JNI 派发)。
- **E2E**:`RegCBackendE2E.IntrinsicMathAndParseIntRoundTrip`(JDK/MSVC 可用时真编 DLL,
  Java 基准逐项对比 + 生成 C 无 JNI call 断言)。
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

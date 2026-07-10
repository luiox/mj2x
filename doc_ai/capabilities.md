# mj2x — 已实现功能清单 (CAPABILITIES)

> 本文档列举 mj2x 当前**已实现且可用**的功能。
> 用法见 `doc/user_guide.md`;路线图见 `doc/roadmap.md`;待办见 `dev_plan.md`。
>
> 能力边界对应 `dev.3` 的实现(2026-07-06 冻结),权威范围见 `doc/mvp_release_scope.md`。

---

## 1. 输入 / 输出

| 能力 | 状态 | 验证 |
|---|---|---|
| 单 `.class` 输入 → 改写后 `.class` + emitted C | ✅ | StackC / RegC E2E |
| `.jar` 输入 → 改写后 `.jar` + emitted C | ✅ | `mj2x_unittest` JAR pipeline 测试 |
| StackC 单 `.class` 输出附带 `mj2x-cloader` sidecar class | ✅ | StackCBackend E2E |
| StackC `.jar` 输出注入 `mj2x-cloader` dispatch loader class | ✅ | JAR pipeline / CLI 测试 |
| JAR 输出保留 entry 顺序和非 class 资源 payload | ✅ | JAR fixture 测试 |
| class entry 在 native method 注入后重新写回 | ✅ | JAR pipeline 测试 |
| `--native-annotation Lpkg/Anno;` 只翻译带目标方法注解的方法 | ✅ | CLI 注解筛选测试 |
| 提供 `mj2x.Native` Java 注解源码包 | ✅ | javac + CLI 注解筛选测试 |
| `--compile` 将 `--emit-c` 结果编译为同目录 native library | ✅ | CLI 编译错误/可用工具链 smoke 测试 |
| `--generate-cmake <dir>` 输出独立 CMake native 工程 | ✅ | `CliTest.GenerateCMakeProjectEmitsStandaloneFiles` |
| `--generate-xmake <dir>` 输出独立 xmake native 工程,`mj2x-cli` 按 `mj2x-crt/resources.manifest` 构建期内嵌并释放 CRT 源文件/模板资源,可支持单 exe 使用 | ✅ | `CliTest.GenerateXmakeProject*` |
| StackC/RegC class 和 jar 重复生成稳定性 | ✅ | `CliTest.ClassInputRepeatableForStackCAndRegC`、`CliTest.JarInputRepeatableForStackCAndRegC`、`tools/product_sample_smoke.ps1 -Mode fast` |

---

## 2. StackC(广覆盖正确性后端)

定位:正确性基线后端,覆盖广,输出仍是 stack-style(`union jvalue stack[]` + `sp`)。
Native 绑定使用 `static` C 函数 + `JNI_OnLoad` 预注册 `mj2x-cloader`
dispatch bridge + `RegisterNatives`,业务方法和注册桥均不产生 per-class `Java_*`
导出符号。单 `.class` 输出在目标目录旁写出 `mj2x/cloader/Loader.class`,JAR 输出注入
同名 loader class。生成方法体内的业务类解析使用当前 owner class 的 `ClassLoader`，覆盖
自定义 class loader 下 `new` / `anewarray` / `multianewarray` / 静态成员 / type check
等路径，bootstrap 异常类和注册入口仍使用 JVM `FindClass`。生成方法体跟踪临时 JNI
local refs，并在正常返回或异常提前返回前批量释放；对象返回值作为 keep ref 保留。
字段和方法 ID 在各生成调用点用 `static jfieldID` / `static jmethodID` 懒缓存,读写由
owner class monitor 保护。hidden proxy class cache 使用 `jweak` 弱全局引用,调用前通过
`NewLocalRef` 提升。字符串 LDC 常量去重到 TU 级 `_mj2x_string_pool[]`，方法体只引用 pool offset。

**已覆盖的 javac 输出形状**:

- 算术:整数 / 长整型 / 浮点运算
- 类型转换:`i2b` / `i2s` / `i2c` / `i2l` 等窄化与拓宽
- 控制流:`if*` / `goto` / switch (`tableswitch` / `lookupswitch`)
- 字段读写:`getfield` / `putfield` / `getstatic` / `putstatic`
- 对象:`new` / `checkcast` / `instanceof`
- 方法调用:`invokevirtual` / `invokespecial` / `invokestatic` / `invokeinterface`
- interface default 方法:原 default 方法保留 Java stub,调用 `DefineClass` 注入的 hidden
  proxy class;native 只注册 proxy 方法,不直接把 interface default 标成 `ACC_NATIVE`
- 数组:`newarray` / `anewarray` / `multianewarray` / `arraylength` / `*aload` / `*astore`;
  `BALOAD`/`BASTORE` 对 `byte[]`/`boolean[]` 做运行时类型分流
- 栈形状操作:`POP` / `POP2` / `DUP` / `DUP_X1` / `DUP_X2` / `DUP2` /
  `DUP2_X1` / `DUP2_X2` / `SWAP`;StackC 通过转换期类型快照区分 category-2
  与 category-1 pair
- 异常:`athrow` / try-catch / finally / monitor (`monitorenter` / `monitorexit`)
- 显式 NPE trap:field / array / instance invoke 在 JNI 调用前主动抛
  `NullPointerException`,并进入当前 catch 分派或按返回类型传播
- int/long DIV/REM 除零:`ArithmeticException` trap,含 catch 分派与未捕获传播
- owner ClassLoader-aware 类解析:应用类查找走 `_mj2x_find_class(env, _mj2x_owner_class, ...)`
- JNI local-ref 追踪:临时 class/string/object/array refs 登记到方法级列表，返回前释放
- JNI method/field ID 懒缓存:调用点首次解析,owner class monitor 保护缓存读写,后续复用缓存 ID
- hidden proxy class cache:使用 WeakGlobalRef,不因 native cache 强持有阻止类卸载
- String pool:重复 LDC 字符串只落一份 `_mj2x_string_pool[]` 数据，方法体按 offset 物化 jstring
- StackC `invokedynamic`:native 侧调用 bootstrap,经 `CallSite.getTarget()` +
  `MethodHandle.invokeWithArguments` 执行;已覆盖 javac string concat、方法引用和捕获 lambda

**验证**:28 个 StackCBackend 真 E2E(Java → mj2x → C → DLL → Java 行为对比)和 StackCBackend
发射单测。

**已知限制**:

| 限制 | 说明 |
|---|---|
| 栈全用 `union jvalue` 数组 | 性能低于寄存器方案,后续可优化 |
| RegC `INVOKEDYNAMIC` fail-closed | RegC/SSA 仍拒绝未展开 indy;StackC 已有运行时 MethodHandle 桥 |
| JSR / RET 不纳入 | 废弃字节码,由 preprocessor / 输入规范处理 |
| `<init>` / `<clinit>` 不被 MarkAll 标记 | 构造器保留原字节码,不走 native |

---

## 3. RegC(保护导向无栈后端)

定位:保护导向后端,抗 c2j 分析。流水线:L1 stack MIR → SSA → RegIR → RegC。

**结构断言(输出保证)**:RegC 输出不出现 `cstack` / `stack[]` / `union jvalue` / `sp--` 这类 stack-machine anchor。

**已覆盖的 E2E 形状**:

- 纯计算(算术、转换)
- 分支 / 循环
- switch
- 字段读写
- 对象创建与操作
- 方法调用
- 数组操作
- 窄类型(`i2b` / `i2s` / `i2c`)
- throw
- try / catch
- null trap
- integer div / rem trap(走 R3 runtime ABI)
- generated C TU 级 static/instance method、static/instance field、class cache CSE:跨方法复用相同 JNI ID/class cache declaration;String intrinsic class cache、固定异常类 class cache、`new`/reference-array allocation 的编译期 descriptor class cache 也统一入池
- **JDK intrinsic(Math / parseInt/parseLong / wrapper toString/compare/value-unbox / Random next* / String read hot paths / StringBuilder 非逃逸链)**:RegC 在 Call 分支识别 JDK 方法签名,匹配调用点不发 `_mj2x_regc_call_*` 通用 JNI helper,直接内联 runtime/local intrinsic

**JDK intrinsic 覆盖(初版,匹配调用点无通用 JNI helper 派发)**:

- `java.lang.Math`:abs/min/max(int/long/float/double)、sin/cos/tan/sqrt/pow、floor/ceil/round、random
- `java.lang.Integer.parseInt(String[,radix])`:native digit parser 解析,NumberFormatException 经 context 回调路由
- `java.lang.Long.parseLong(String[,radix])`:native digit parser 解析,NumberFormatException 经 context 回调路由
- `java.lang.Integer/Long/Short/Byte/Float/Double/Boolean/Character.toString`、`java.lang.Integer/Long/Short/Byte/Float/Double/Boolean/Character.compare`:无状态 compare 与基础整型/布尔/字符 `toString` 走 `mj2x-crt` C ABI;`Float/Double.toString` 复用缓存的 JDK `String.valueOf(F/D)` 保持浮点文本格式,`Float/Double.compare` 显式处理 NaN 与 signed zero 语义
- `java.lang.String.valueOf(int|long|boolean|char|float|double|Object)`:RegC local helper 覆盖常见值转字符串;`float|double` 和 `Object` 通过缓存的 JDK methodID 保持浮点格式和虚分派语义
- String LDC:经 `_mj2x_intrinsic_string_literal` 物化为 jstring,供 intrinsic 路径消费
- `java.lang.String`:equals/length/charAt/hashCode/isEmpty/startsWith(String)/endsWith(String)/indexOf(String)/contains(CharSequence)
  走 RegC local helper;返回值被丢弃的调用点也可安全发射 intrinsic call;`contains` 对 String rhs 走 native `indexOf`,非 String `CharSequence` 回退到缓存的 Java `String.contains` methodID
- `java.lang.StringBuilder` 非逃逸第一片:`new`/`<init>()`/`<init>(String)` +
  `append(String|Object|CharSequence|int|long|boolean|char|char[]|float|double)` + `toString`,RegC 用固定容量 native shadow buffer 承载,
  其中 `append(float|double)` 复用缓存的 `String.valueOf(F/D)` 保持 JDK 浮点文本格式,
  匹配链式调用不发 `_mj2x_regc_call_*`;shadow state 逃逸仍 fail-closed
- 包装类非逃逸 value/unbox:`Integer/Long/Short/Byte/Float/Double/Boolean/Character.valueOf`
  后接对应 `xxxValue`,RegC 用 native shadow struct 承载,不发 JNI call
- `java.util.Random` 非逃逸 new/init/next*:`mj2x_random` shadow state + runtime LCG,
  nextInt/nextLong/nextBoolean/nextFloat/nextDouble 不发 JNI call

**intrinsic 限制(初版)**:包装类/StringBuilder native shadow object 一旦返回、存字段/数组、传入未知 call
或跨 CFG edge 会 fail-closed;Random shadow state 同样只支持非逃逸路径。StringBuilder first slice 还不是完整 Java 语义:内部固定 1024 字节 UTF-8 friendly buffer,后续需补 UTF-16 ownership、扩容、更多 append 重载和逃逸 materialize。

**CLI**:

- `--backend regc`,支持 `.class` 与 `.jar` 输入输出路径。
- `--native-annotation <desc>` 启用方法级筛选,descriptor 使用 JVM 内部注解格式(如 `Lmj2x/Native;`)。未指定时仍使用 `MarkAllMethodsPreprocessor` 默认标记所有可翻译方法。
- `mj2x/mj2x-annotations/src/main/java/mj2x/Native.java` 提供用户可直接引入的 `@mj2x.Native` 注解源码。
- `--compile` 在 `--emit-c` 之后自动调用 C 编译器生成 `.dll` / `.so` / `.dylib`,输出到 `--output` 同目录;`--c-compiler msvc|gcc|clang` 可覆盖自动检测。
- 编译阶段从 `JAVA_HOME` 推导 JNI include 路径;Windows/MSVC 环境未初始化时会尝试调用 Visual Studio 2022 `vcvarsall.bat`;`JAVA_HOME` 缺失、JNI include 缺失、`vcvarsall.bat` 缺失或编译器退出失败时返回非零并打印明确错误。
- `--generate-cmake <dir>` 生成 `native.c`、`CMakeLists.txt`、`.gitignore` 和 `README.md`;
  CMake 工程通过 `find_package(JNI REQUIRED)` 发现 JNI headers。

---

## 4. R3 Runtime 基础

| 模块 | 状态 |
|---|---|
| runtime core(crypto / kernel / stringpool / antidebug / antijvmti) | ✅ 单测模块 |
| `mj2x-crt` 可分发 RegC ABI | ✅ C99/JNI source pack + `mj2x_crt_test`;无 `libca` 分发依赖 |
| RegC / R3 ABI 冻结 | ✅ 当前冻结在 div / rem / barrier、compare、基础 `toString` helper |
| runtime-backed div / rem trap | ✅ RegC native E2E 覆盖 |
| **JDK intrinsic runtime**(intrinsic/wrapper、intrinsic/math、intrinsic/random) | ✅ 单测模块,RegC native E2E 覆盖 |
| Math intrinsic(abs/min/max/sin/cos/tan/sqrt/pow/floor/ceil/round/random) | ✅ runtime + emitter 识别 |
| Integer.parseInt / Long.parseLong radix overloads / wrapper toString / wrapper compare intrinsic | ✅ parseInt 与复杂格式仍为 emitter/runtime 路径;无状态 compare/基础 `toString` 由 CRT 提供 + E2E |
| 包装类 native struct(Integer/Long/Short/Byte/Float/Double/Boolean/Character)valueOf/xxxValue | ✅ runtime + RegC 非逃逸 value/unbox;逃逸 fail-closed |
| Random Java 兼容 LCG(nextInt/nextLong/nextDouble/nextBoolean/nextInt(bound)) | ✅ runtime + RegC 非逃逸 new/init/next*;逃逸 fail-closed |
| String intrinsic 最小 UTF-16 view 垫片(length/charAt/status) + RegC String read hot paths | ✅ runtime 单测 + RegC emitter/结构测试 + native E2E |
| StringBuilder 非逃逸 append/toString first slice | ✅ RegC emitter/结构测试 + native E2E;当前为本地固定 buffer,CRT 已定义 opaque UTF-16 C ABI 供后续替换;覆盖 `append(char[])`;逃逸 fail-closed |
| antidebug 软件断点字节扫描 | ✅ mockable runtime 单测 |
| antijvmti agent/JDWP 参数、模块名、dump capability 信号聚合 | ✅ mockable runtime 单测 |

---

## 5. 项目结构 + codegen 工具层

**模块化目录**(对齐 `libs/libjbytecode/<module>/` 子库模式):

```
mj2x/
├── mj2x-core/src/mj2x/      核心库(include 根 = src,内层 mj2x/ 保留 → 98 处 include 零改动)
├── mj2x-cli/src/             CLI 入口
├── mj2x-crt/src/             可分发 C99/JNI RegC ABI source pack
├── mj2x-runtime/src/         in-tree native runtime(crypto/kernel/intrinsic/...)
├── unittest/src/             单测
```

| 能力 | 状态 |
|---|---|
| codegen 工具层(`mj2x-core/src/mj2x/codegen/`) | ✅ CSourceStream(缩进管理)、IncludeSet(include 去重)、cStringLiteral/mangle 收口 |
| StackC 拆到 `backend/stackc/` 文件夹 | ✅ stackc_backend + stackc_runtime_prelude 分离 |
| 重复 helper 收口(cStringLiteral/mangle 跨 StackC/RegC) | ✅ 单一来源在 codegen |

**codegen 工具层设计**:替代裸 `std::ostringstream << "    ..."` 字符串拼接。`CSourceStream` 管缩进、`IncludeSet` 管 include 去重排序、`c_helpers.hpp` 是无状态 helper 的单一来源。两个 C 发射后端(StackC/RegC)共用。`StackCBackend` 的 registration 与 method/field ID 懒缓存发射已迁到 `CSourceStream`;后续大段 runtime helper 字符串迁移是渐进工作。

## 6. 能力索引

| 能力域 | 当前权威来源 |
|---|---|
| 字节码转 native | `mj2x/mj2x-core/src/mj2x/backend/{stackc,regc}/` |
| 可分发 RegC 运行时 ABI | `mj2x/mj2x-crt/src/regc/` + `mj2x/mj2x-crt/resources.manifest` |
| native 运行时 | `mj2x/mj2x-runtime/src/` |
| RegC JDK intrinsic | `mj2x/mj2x-core/src/mj2x/backend/regc/` + `mj2x/mj2x-runtime/src/intrinsic/` + `mj2x/mj2x-crt/src/regc/` |
| 方法选择与 CLI native 编译 | `mj2x/mj2x-cli/` |
| 用户注解包 | `mj2x/mj2x-annotations/` |
| protected samples | `mj2x/test_resource/protected_samples/` |

---

## 7. 验证入口

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x-crt
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_crt_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

能力到测试的追溯见 `../doc_ai/mvp_backend_verification.md`。

## 8. Validation maturity

- 首切片：`script/validate_release.ps1 -Layer mj2x -Mode release` 覆盖 build、unit、CLI smoke，并产出 `build/artifacts/windows/x64/<mode>/mj2x/mj2x-windows-x64-<mode>.zip` 与 `SHA256SUMS`。
- 尚未产品级：真实 protected sample corpus、跨 MSVC/GCC/Clang native compile gate、mjdbg observation report 仍需作为 release gate 固化。

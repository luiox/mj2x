# mj2x — 已实现功能清单 (CAPABILITIES)

> 本文档列举 mj2x 当前**已实现且可用**的功能。
> 用法见 `doc/user_guide.md`;路线图见 `doc/roadmap.md`;待办见 `dev_plan.md`。
>
> 能力边界对应 `dev.3` 的实现(2026-07-06 冻结),权威范围见 `doc/mvp_release_scope.md`。

---

## 1. 输入 / 输出

| 能力 | 状态 | 验证 |
|---|---|---|
| 单 `.class` 输入 → 改写后 `.class` + emitted C | ✅ | CBackend / RegC E2E |
| `.jar` 输入 → 改写后 `.jar` + emitted C | ✅ | `mj2x_unittest` JAR pipeline 测试 |
| JAR 输出保留 entry 顺序和非 class 资源 payload | ✅ | JAR fixture 测试 |
| class entry 在 native method 注入后重新写回 | ✅ | JAR pipeline 测试 |
| `--native-annotation Lpkg/Anno;` 只翻译带目标方法注解的方法 | ✅ | CLI 注解筛选测试 |
| 提供 `mj2x.Native` Java 注解源码包 | ✅ | javac + CLI 注解筛选测试 |
| `--compile` 将 `--emit-c` 结果编译为同目录 native library | ✅ | CLI 编译错误/可用工具链 smoke 测试 |
| `--generate-cmake <dir>` 输出独立 CMake native 工程 | ✅ | `CliTest.GenerateCMakeProjectEmitsStandaloneFiles` |

---

## 2. CBackend(广覆盖正确性后端)

定位:正确性基线后端,覆盖广,输出仍是 stack-style(`union jvalue stack[]` + `sp`)。
Native 绑定使用 `static` C 函数 + `JNI_OnLoad` / `RegisterNatives`,不为每个 Java 方法导出
`Java_*` 符号。生成方法体内的业务类解析使用当前 owner class 的 `ClassLoader`，覆盖
自定义 class loader 下 `new` / `anewarray` / `multianewarray` / 静态成员 / type check
等路径，bootstrap 异常类和注册入口仍使用 JVM `FindClass`。生成方法体跟踪临时 JNI
local refs，并在正常返回或异常提前返回前批量释放；对象返回值作为 keep ref 保留。
字段和方法 ID 在各生成调用点用 `static jfieldID` / `static jmethodID` 懒缓存。字符串
LDC 常量去重到 TU 级 `_mj2x_string_pool[]`，方法体只引用 pool offset。

**已覆盖的 javac 输出形状**:

- 算术:整数 / 长整型 / 浮点运算
- 类型转换:`i2b` / `i2s` / `i2c` / `i2l` 等窄化与拓宽
- 控制流:`if*` / `goto` / switch (`tableswitch` / `lookupswitch`)
- 字段读写:`getfield` / `putfield` / `getstatic` / `putstatic`
- 对象:`new` / `checkcast` / `instanceof`
- 方法调用:`invokevirtual` / `invokespecial` / `invokestatic` / `invokeinterface`
- 数组:`newarray` / `anewarray` / `multianewarray` / `arraylength` / `*aload` / `*astore`;
  `BALOAD`/`BASTORE` 对 `byte[]`/`boolean[]` 做运行时类型分流
- 异常:`athrow` / try-catch / finally / monitor (`monitorenter` / `monitorexit`)
- int/long DIV/REM 除零:`ArithmeticException` trap,含 catch 分派与未捕获传播
- owner ClassLoader-aware 类解析:应用类查找走 `_mj2x_find_class(env, _mj2x_owner_class, ...)`
- JNI local-ref 追踪:临时 class/string/object/array refs 登记到方法级列表，返回前释放
- JNI method/field ID 懒缓存:调用点首次解析，后续复用缓存 ID
- String pool:重复 LDC 字符串只落一份 `_mj2x_string_pool[]` 数据，方法体按 offset 物化 jstring
- CBackend `invokedynamic`:native 侧调用 bootstrap,经 `CallSite.getTarget()` +
  `MethodHandle.invokeWithArguments` 执行;已覆盖 javac string concat、方法引用和捕获 lambda

**验证**:28 个 CBackend 真 E2E(Java → mj2x → C → DLL → Java 行为对比)和 CBackend
发射单测。

**已知限制**:

| 限制 | 说明 |
|---|---|
| method/field ID 懒缓存未加锁 | 多线程首次同时命中同一调用点时可能重复解析一次 |
| 栈全用 `union jvalue` 数组 | 性能低于寄存器方案,后续可优化 |
| RegC `INVOKEDYNAMIC` fail-closed | RegC/SSA 仍拒绝未展开 indy;CBackend 已有运行时 MethodHandle 桥 |
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
- **JDK intrinsic(Math / parseInt)**:RegC 在 Call 分支识别 JDK 方法签名,不发 JNI,直接内联 runtime intrinsic

**JDK intrinsic 覆盖(初版,无 JNI 派发)**:

- `java.lang.Math`:abs/min/max(int/long/float/double)、sin/cos/tan/sqrt/pow、floor/ceil/round、random
- `java.lang.Integer.parseInt(String)`:native strtol 解析,NumberFormatException 经 context 回调路由
- String LDC:经 `_mj2x_intrinsic_string_literal` 物化为 jstring,供 intrinsic 路径消费

**intrinsic 限制(初版)**:包装类对象 valueOf/intValue、Random nextInt 涉及对象逃逸表示,暂走 JNI(后续扩展)。

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
| RegC / R3 ABI 冻结 | ✅ 当前冻结在 div / rem / barrier helper |
| runtime-backed div / rem trap | ✅ RegC native E2E 覆盖 |
| **JDK intrinsic runtime**(intrinsic/wrapper、intrinsic/math、intrinsic/random) | ✅ 单测模块,RegC native E2E 覆盖 |
| Math intrinsic(abs/min/max/sin/cos/tan/sqrt/pow/floor/ceil/round/random) | ✅ runtime + emitter 识别 |
| Integer.parseInt intrinsic | ✅ emitter 内联 JNI helper + E2E |
| 包装类 native struct(Integer/Long/Short/Byte/Float/Double/Boolean/Character)valueOf/xxxValue | ✅ runtime(对象逃逸未接 emitter) |
| Random Java 兼容 LCG(nextInt/nextLong/nextDouble/nextBoolean/nextInt(bound)) | ✅ runtime(逐位对齐 JDK,对象逃逸未接 emitter) |

---

## 5. 项目结构 + codegen 工具层

**模块化目录**(对齐 `libs/libjbytecode/<module>/` 子库模式):

```
mj2x/
├── mj2x-core/src/mj2x/      核心库(include 根 = src,内层 mj2x/ 保留 → 98 处 include 零改动)
├── mj2x-cli/src/             CLI 入口
├── mj2x-runtime/src/         native runtime(crypto/kernel/intrinsic/...)
├── unittest/src/             单测
```

| 能力 | 状态 |
|---|---|
| codegen 工具层(`mj2x-core/src/mj2x/codegen/`) | ✅ CSourceStream(缩进管理)、IncludeSet(include 去重)、cStringLiteral/mangle 收口 |
| CBackend 拆到 `backend/cbackend/` 文件夹 | ✅ c_backend + c_runtime_prelude 分离 |
| 重复 helper 收口(cStringLiteral/mangle 跨 CBackend/RegC) | ✅ 单一来源在 codegen |

**codegen 工具层设计**:替代裸 `std::ostringstream << "    ..."` 字符串拼接。`CSourceStream` 管缩进、`IncludeSet` 管 include 去重排序、`c_helpers.hpp` 是无状态 helper 的单一来源。两个 C 发射后端(CBackend/RegC)共用。后续 emitRuntimeHelpers/emitHelperPrelude 的大段字符串迁移到 CSourceStream 是渐进工作。

---

---

## 5. 生成 xmake 工程

`--generate-project <dir>` 把转 C 结果连同 `xmake.lua` / `.gitignore` / 原 class / (RegC) runtime 源码打包成独立可编译工程。

> 注:该功能当前在分支 `feat/mj2x/xmake-project-generator`,实现为 emit 后字符串改写,待 golden test 建立后重构到 codegen 层。详见分支提交说明。

---

## 6. 验证入口

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
```

能力到测试的追溯见 `../doc_ai/mvp_backend_verification.md`。

# dev_plan：mj2x 施工计划

> 本文件只保留**待做项**与**状态总览**；已完成阶段的详细落地记录已归档到
> 对应 PR / git log（见文末"已完成归档"索引）。依据 issue #787 §3（mj2x 部分）。

## 状态总览

| 主线 | 内容 | 状态 |
|---|---|---|
| 一~五、八~十一 | VMC4 可用版 / RegC 性能（含方法内联）/ 异常精确化 / 反侦测体系 / 文档 / 用户 intrinsic（#818）/ program 粒度（#827）/ 混淆借鉴与默认关（#829） | ✅ 完成（见归档索引） |
| 六（剩余 issue） | VMC4 升级档：period 语义变义（#762/#844）已实现，N-7 对抗全闭环 | 🟡 S2 对抗验证 + 红队契约扩展待做（见待做 A） |
| 七（#820 测试集） | 步骤 1-5 完成（调研/设计/Wave3/三后端矩阵/性能回归自动化） | 🟡 步骤 6 对抗闭环待做（见待做 B） |
| 十二（配置系统） | P0+P1 完成：参数注册表 + 全量配置化 + `--dump-config-schema`/`--dump-config-template` | 🟡 GUI 配置工具（P2）待做（见待做 D） |
| 十三（LLVM IR 后端） | 基础款转正默认开启（v2.2）+ 二次扩面 11 项（对象/数组/实例调用/异常/monitor/ldc/窄类型等，PR #864） | ✅ 完成（见归档索引） |
| 十四（LLVM 第二轮扩面） | invokedynamic 运行时解析、intrinsic 扩面对齐 RegC、保护特性接入、mtest_set 矩阵 + protected_samples llvm 样本 | 🟢 完成（Math/包装类等 RegC-only intrinsic 下沉除外，见待做 E） |

## 待做项

### A. VMC4 升级档 S2（P2）

- period 语义变义（case 标签周期轮换）已实现（#844）且 mjdbg N-7 对抗闭环就绪
  （五形态识别 + period 变义 label 表提取/case 常量反查 + computed_goto 产物修复，
  见 `mj2x/doc_ai/vmc4_red_team_contract.md` §6）。剩余：开 `feat/mj2x-vmc4-mutation`
  的 S2 对抗验证 + 红队契约扩展（变义强度模型同批）。

### B. mj2x 自建测试集（#820）

- 步骤 6 待做：mjdbg 对抗闭环（每用例 expected trace surface + weakness report，
  与 mjdbg 工具端联动）。

### C. 记录不阻塞（P3）

- VMC4 检查点指令级异常匹配（javac 字节码下块粒度==指令粒度，无实际缺口）、
  agent 特征深度检测（javaagent MANIFEST 扫描 / JDWP 探测）、异常表多 region
  匹配顺序测试扩展、runtime C++ 版与 crt C 版检测面统一。
- JVMRTDP 调研（issue #847）：纯 Java 面 JVMTI 工具，对 mj2x 保护产物（native
  化/VMC4）**无实质威胁**。观察 a：agent 用 manual mapping 加载可绕过「已加载
  模块枚举」（Windows 侧；capability 快照 + 启动参数扫描仍覆盖，记录不实施）；
  观察 b：RegC 无字符串加密变换（对标 jvmtp `obfuscation.strings`），敏感字符串
  场景需加密模块（P3）。

### D. 配置系统 GUI 工具（P2）

- 配置系统 P0+P1 已完成（见归档索引）。设计已完成
  （`doc_ai/gui_config_tool_design.md`：vite+vue 方案、schema 驱动渲染、
  联动/校验/导出/可选 sidecar 执行）。剩余：P1 实现（表单渲染 + 校验 + 导出）。

### E. LLVM 第二轮扩面（已完成，PR #870）

- **invokedynamic 运行时解析** ✅：残留 `indy:<idx>` 站点（自定义 bootstrap）不再
  fail-closed——LLVM 发射模块级全局 cache（`NewLocalRef` 缓存 + `NewGlobalRef`
  稳定化）+ 运行时 bootstrap 解析（`MethodHandles.lookup` / `MethodType` /
  `privateLookupIn` / bsm 调用 / `CallSite.getTarget` / `invokeWithArguments`，
  参数装箱、返回拆箱）。与 RegC 对齐（regc_backend 的 emitIndyBootstrapFunction
  同功能——原文"超出 RegC"失实，2026-08-16 审查订正）。
  验证：`CliTest.LlvmIndyCustomBootstrapE2E`（plain / runtime-protection fake /
  regc-obfuscate 三变体，输出 7-42-11）+ `LlvmIrBackendTest.EmitsIndyRuntimeResolution`。
- **intrinsic 扩面对齐 RegC** 🟡（Math 纯函数 + 包装类 + Random + StringBuilder
  完成）：`java.lang.Math` 全纯函数面直调 mj2x-crt runtime 符号（abs/min/max
  I/J/F/D、sin/cos/tan/sqrt/floor/ceil/pow、round、random；0 JNI、无
  ExceptionCheck，declare 模块级输出，链接侧并入 mj2x-crt 源）；**包装类
  valueOf/xxxValue 非逃逸切片**（Integer/Long/Short/Byte/Float/Double/Boolean/
  Character——shadow 结构 `%mj2x_int` 等 + 窄装箱 trunc / 窄拆箱 sext/zext，
  逃逸根经 copy 联通分析回退 JNI，对齐 RegC regc_intrinsics.cpp）；**Random
  状态机切片**（非逃逸 new Random(seed) + nextXxx 直调——RegC 同 face 为 inert；
  无参构造保持 JNI）；**StringBuilder/StringBuffer 状态机切片**（非逃逸
  new Builder + init()V/(I)V/(String)V/(CharSequence)V + append 全 13 面 +
  toString/length/capacity/charAt/ensureCapacity/setLength/setCharAt/
  appendCodePoint——函数级 `%mj2x_regc_context` 前导（init_context +
  throw_exception 回调）+ 每次调用 mj2x_regc_status 检查（非 0 → 异常分发/
  默认返回），状态为 crt 堆分配 `mj2x_string_builder*`）。String 类面
  （length/charAt/substring/equals 等 RegC-only）保持 JNI 分派，留待整类
  下沉 F。
- **保护特性接入** ✅：`--runtime-protection` 支持 llvm——每方法入口注入
  `%mj2x_rt_checkpoint`（模块级全局 + `mj2x_rt_checkpoint_run` 调用，crt
  `mj2x_rt_protect.c` 在 link/工程层追加；FAKE 动作走默认返回路径）。
  `--regc-obfuscate` 及 MBA/opaque/CFF 变换选项支持 llvm（RegIR 级同源变换；
  mbaDeep 的 Barrier 指令 LLVM 子集不支持时回退未变换形态并告警）。
  `--regc-safe-env` 明确不适用（JNI 表快照恢复是 RegC 运行时面）。
- **测试矩阵接入** ✅：`mtest_set_native_matrix_test.cpp` 加 LLVM 列（22 fixtures
  允许清单，fail-closed 自动排除未列入者；clang 探测缺失时整列跳过）；
  `protected_samples/` 增加 llvm 样本目录（Tiny/IndySurface/ExceptionMonitor 三样本，
  `LlvmSamples.GenerateMjdbgFixtures` 生成 native.ll + clang 编译 + Driver 真跑）。

### F. 整类下沉（P3，记录不阻塞）

- roadmap §2 表格行已列（算法密集类整体 native 化，边界保留 JNI）；GUI 设计已
  预留"方法/类选择"扩展点（`gui_config_tool_design.md` §6）。条件：无反射、无
  动态类加载、intrinsic 覆盖充分；先做单类试点再铺开。

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。

## 日志

- 2026-08-17（regc 13-probe 性能第二轮：整体反超 os30）：**Total ratio
  1.07→0.95（116ms vs os30 122.3ms，MSVC -p windows 口径），首次整体反超**。
  两项优化：`getClass().getName().hashCode()` 链融合 intrinsic（emitter 前瞻
  匹配 + CRT 双槽 receiver 身份缓存 + 自旋锁，3 JNI→1，Inheritance
  1.72→1.55、Reflection 1.38→1.26）；`substring(I|II)` intrinsic 回退
  （CRT 版本 4 JNI + 全串拷贝劣于 1-JNI JDK 调用，String 1.54→1.12）。
  基准脚本修复：UTF-8 BOM（PS5.1 ANSI 解析炸中文）、`-P` 移到 `build`
  子命令后（本机 xmake 全局选项前置报 invalid argument）、`java -version`
  经 cmd /c 合并 stderr（PS5.1 NativeCommandError）。验证：mj2x_unittest
  613 过/3 skip、mtest_set 25/25、E2E 套件绿、覆盖审计 RegC 74.7% vs
  os30 61.6%。遗留：Inheritance 1.55（bias 字段/checkcast/接口分派）、
  Exception 1.37、ControlFlow 1.34、Crypto 1.04（JDK 侧主导）。
- 2026-08-17（regc 13-probe 性能追平 os30）：**Total ratio 1.14→0.98（118.8ms
  vs os30 121.6ms），7/13 探针反超**（Enums 0.53x / Visitor 0.77x / Reflection
  0.91x / Resource 0.15x / ClassLoading 0.67x / Collections 0.98x / Inner 1.00x）。
  分支 `perf/regc-probe-optimization`，三项优化（详见
  `benchmark/results/2026-08-17-jvm-obf-tester-os30.md`）：枚举 intrinsic
  （子类 name/ordinal 直连 + values/valueOf javac 合成形态内联，后端预扫描
  在 injectIntoClass 清空指令前验证方法体）；StringMethod 亏损 intrinsic 回退
  （hashCode/charAt/equals 的 3-6 JNI 数据管道不如 1-JNI 缓存调用 + HotSpot
  VM intrinsic；反直觉教训：GetStringUTFChars ASCII 快路径实测也比 3 个 UTF16
  调用贵，已试并回退）；确定性虚调用免检直调（final/无覆写时跳过 IsInstanceOf，
  含匿名类 ACC_FINAL 缺失修正与 convertedFunctions 声明类回查）。验证：
  mj2x_unittest 全量绿（CliTest 3 例为 shell 环境 MSYSTEM/PATH 污染 xmake
  平台检测所致，`env -u MSYSTEM` 后全过）；E2E 13/13。遗留：Inheritance
  1.41x（checkcast 复用/类名 hash 缓存）、Crypto 1.21x（JDK 侧主导）。
  另修复 main 上 native_compiler.cpp environmentKeyEquals 链接错误。
- 2026-08-21（round-2 收尾·toString 修正）：**`StringBuilder.toString()` 映射修正为
  to_jstring**——实时隔离定位（非猜测）：独立 hex Driver 反调 `concat()`，返回
  `java.lang.StringBuilder` 对象（`CLASS=java.lang.StringBuilder`）而非 String；
  `concat("k",7)` 单独调用即 len=16、'k'=7 + 11 个 NUL。根因：LLVM intrinsic
  registry 把 `StringBuilder.toString` 误映射到 `mj2x_string_builder_materialize`
  （转义桥，返回真 Java StringBuilder 对象，头注释明示），而 Java 语义要求
  toString 返回真 String。修复：toString → `mj2x_string_builder_to_jstring`
  （真 jstring），StringBuffer 同步 `mj2x_string_buffer_to_jstring`，单测断言跟随。
  证据链：修复前 `.ll` 与正确样本 hash 相同（emitter 无错、缺陷在 crt 调用选择）；
  修复后 `LlvmIndySurfaceSample/native.ll` 已 `call i32 @mj2x_string_builder_to_jstring`；
  `LlvmSamples.GenerateMjdbgFixtures` PASSED（`[k]=7 slice=5 types=true`）。提交 1b6f4f721。
- 2026-08-21（round-2 收尾·全量验证与文档）：`mj2x_unittest` 全量 **600 过 / 3 skip /
  0 fail**（58 suites、707s；3 skip = EmitsExceptionDispatchWithRegions 由 CliExc E2E
  覆盖 + RealZig* 本机无 zig）；`mtest_set_unittest` **25/25 过**；`mj2x_integration_test`
  **66 过 / 1 skip**（67 例：MatrixBaseline 语义矩阵含 LLVM 列全绿；skip = 性能回归门控
  `MJ2X_PERF_REGRESSION` 未设）。分组回归全绿：LLVM 32 过 1 skip、RegC 108、StackC 83、
  VMC4 145。`CliTest.GenerateXmakeProjectUsesEmbeddedCrtOutsideRepo` 本机为环境性偶发
  （spawn 的真实 xmake 退出 0xC0000374 堆损坏；该测试早于本 PR，手工独立工程 xmake
  f+build 均 exit 0，全量跑中亦通过），判定非回归。文档同步：roadmap §2 Intrinsic 行 +
  §4 LLVM 状态行（PR #870 完成态）、user_guide §5.4（invokedynamic 运行时解析 / intrinsic
  扩面对齐 RegC / 保护特性 / to_jstring，fail-closed 列表剔除 invokedynamic）；capabilities
  无回退。
- 2026-08-21（round-2 收尾·补 intrinsic 4）：**StringBuilder/StringBuffer 状态机
  切片**——非逃逸 builder 全链直调 crt（init 四面 + append 十三面 +
  toString/length/capacity/charAt/ensureCapacity/setLength/setCharAt/
  appendCodePoint；函数级 `%mj2x_regc_context` 前导 + 每次调用 mj2x_regc_status
  检查；append 结果与 receiver 联通；修复 init 的 out_state 参数顺序
  （crt 签名 (context, args..., out_state**)，曾把槽放参数前导致写穿 jstring））。
  验证：E2E 与 JVM 逐位一致 `b=7,...,d=3.2` + `len=7`。
- 2026-08-21（round-2 收尾·补 intrinsic 3）：**Random 状态机切片**——非逃逸
  `new Random(seed)` + nextInt/nextInt(bound)/nextLong/nextBoolean/nextFloat/
  nextDouble 直调 mj2x-crt（`%mj2x_random = { i64 }` 指针状态；标准 Java LCG，
  E2E 与 JVM 逐位一致 `sum=1008396158678580558 b=36`；无参构造保持 JNI；
  RegC 同 face 为 inert——Allocate+<init> 融合 NewObjectA 绕过 registry）。
  修复：fallback 扫描漏 randomRoots（ARETURN 逃逸未回退）；Random 结构定义
  未推入模块级声明（clang Cannot allocate unsized type）。
- 2026-08-21（round-2 收尾·补 intrinsic 2）：**包装类 shadow 切片**——Integer/
  Long/Short/Byte/Float/Double/Boolean/Character 的 valueOf/xxxValue 非逃逸用法
  直调 mj2x-crt 值结构（`%mj2x_int` 等单字段 struct；窄装箱 trunc、窄拆箱
  sext/zext；valueOf 结果寄存器经 Copy 联通逃逸分析，任何非匹配拆箱使用整根
  回退 JNI；Short/Byte 的 valueOf 用 JVM 实际窄描述符 (S)/(B)）。验证：单测
  EmitsWrapperBoxUnboxDirectCalls + E2E `sum=23 esc=7`（六种 wrapper 直调 +
  逃逸回退）。
- 2026-08-21（round-2 收尾·补 intrinsic）：**Math 纯函数切片**直调 mj2x-crt
  runtime（abs/min/max I/J/F/D、sin/cos/tan/sqrt/floor/ceil/pow、round、random；
  0 JNI、无 ExceptionCheck，`.ll` 模块级 declare）；LLVM `--compile`（pipe/文件）
  并入 mj2x-crt 源（rt_protect 专用路径单一提供，修 LNK2005 重复符号）；
  `--generate-cmake/--generate-xmake` 工程整树分发 crt。验证：mj2x_unittest
  594 过 / 3 skip；矩阵 66 例全绿（g5.abs 走 intrinsic）；Math E2E
  `abs=10 sqrt=5.0`（pipe/文件双模式）。
- 2026-08-21（round-2 收尾）：四主线落地——**indy 运行时解析**（自定义 bootstrap
  `indy:<idx>` → 全局 cache + bsm resolver，与 RegC 对齐——原文"超出 RegC"失实，订正）、**intrinsic**（cmp.nan_*）、
  **保护特性**（runtime-protection 检查点 + MBA/opaque/CFF 变换接入 llvm）、
  **测试矩阵**（LLVM 列 22 fixtures + protected_samples/llvm 三样本）。修复：
  ANEWARRAY 数组描述符悬垂（SSA arena 生命周期）、I2S 误按 i8 截断、check_cast/
  instance_of 参数缺 ptr 类型、mangle `$`→`_00024`。验证：mj2x_unittest 全绿 +
  mtest_set 25 过 + 矩阵 66 例全绿 + 样本 E2E（见 PR #870）。
- 2026-08-21（round-2 开工）：同步 roadmap 两处 stale（arraycopy intrinsic 已完成
  #819、方法内联矛盾）+ §4 LLVM 状态行（#864 扩面后）；GUI 配置工具设计完成
  （vite+vue，`doc_ai/gui_config_tool_design.md`）；记录整类下沉为 P3 待办（待做 F）。
- 2026-08-21：LLVM IR 后端二次扩面 11 项全部落地（PR #864）——**对象**（REF/字段
  读写/分配/类型检查）、**数组**（全 XALOAD/XASTORE/arraylength/多维）、**调用**
  （实例/外部 JNI CallXxxMethodA 分派）、**异常真实现**（放行异常区域 + 函数级
  dispatch label + 除零分发；修复此前异常方法静默回退字节码的假阳性）、**monitor**、
  **ldc String/Class**、**intrinsic 面对齐**（array.length/instanceof/monitor，
  invokedynamic 除外）、**窄类型 Z/B/C/S**、**浮点特殊字面量**、**工具链**
  （pipe/cmake/xmake）。NewStringUTF 槽位按 jni.h 成员数核对 163→167。单文件后端
  拆分五模块（llvm_ir_internal.hpp/validation/emitter/helpers/backend）。验证：
  mj2x_unittest 585 过 3 skip；mtest_set_unittest 25 过；真 native E2E
  `234-7-9-42-hi` 与 `9-20-5-18-15-wrapped`（多 catch/显式 throw/finally/除零重抛）。
- 2026-08-19：LLVM IR 后端基础款二次扩面——**整数除/取余**（udiv/urem 位模式
  等价规避 INT_MIN/-1 溢出 poison；除零经共享 throw-arithmetic helper 抛
  ArithmeticException：JNIEnv 表槽 FindClass=6/ThrowNew=14，helper 签名统一
  加 JNIEnv* 首参透传）、**递归/互递归**（预检 visiting 命中按描述接受，符号
  确定性生成不悬空）、**跨类静态调用**（目标须非 library + markedForCompile +
  可转换；符号用目标 owner）、**浮点→整数**（llvm.fptosi.sat，NaN→0/溢出饱和）。
  验证：LlvmIrBackendTest 11 例 + cli_test 3 例全绿；E2E 输出
  `17-83-14-2-3628800-2147483647-0-ae`（除法/余数/递归阶乘/f2i 饱和/NaN/除零
  异常捕获，clang/llc 双路径）。
- 2026-08-19：LLVM IR 后端基础款转正并扩面（v2.2 默认开启，`with_mj2x_llvm` 默认
  on）——受限标量子集：静态 int/long/float/double/void 纯计算（含移位，Java 掩码
  语义）+ 比较/分支/循环/switch + **同类内静态互调**（三态 resolve：已编译成功
  按描述解析 / 编译失败整链 fail-closed / 未编译递归预检含环检测）；对象/JNI/异常/
  monitor/内存/整数 div-rem/跨类·实例·递归调用保持 fail-closed。验证：
  LlvmIrBackendTest 7 例 + cli_test 3 例（clang/llc 双路径编译 → JVM 运行 E2E，
  互调+移位输出 17-83）全绿。
- 2026-08-19：mjdbg N-7 VMC4 ISA 恢复工具 v1 全闭环（PR #845）——五形态识别 +
  静态全恢复（跨实例复用率 100%），VMC4 低风险档强度归零、S2 启动条件满足；
  红队验证发现并修复 computed_goto label 定义（`::` → `:`）。
- 2026-08-16（语义审计修复 X1-X4,分支 `mj2x/semantics-hardening`）：
  - **X1（最严重）**：`LdcPreprocessor` 展开栈序缺陷——压栈 `Class,
    String(desc), MethodType, String(name)` 与 `findVirtual/findStatic` 的
    `(Class, String, MethodType)` 不符（多一参且 name/MethodType 颠倒）；
    field tag 沿用 3 参 MethodType 形态 desc、`findSpecial` 4 参只压 3 参、
    `findConstructor` 2 参多压 String——默认注册 + 遍历所有非库类，任何含
    ldc Handle 的类经 CLI 处理即 VerifyError。修复：按 tag 分派重写展开
    （含 field 类型的 Class 发射,primitive 走 `GETSTATIC <Box>.TYPE`）、
    MethodType 构造改用调用者 ClassLoader（保护态 jar 语义）、code_converter
    Handle ldc 由静默 null 改显式 unsupported marker。守门：迷你栈校验器单测
    （11 例,直接抓住压栈顺序/数量错误）+ 手构字节码 exec E2E（javac 不产
    ldc Handle,保护器产物才是真实来源）。
  - **X2**：RegC/LLVM 移位缺 JVM `&31/&63` 掩码（C UB / LLVM poison）,且
    `llvm_ir_validation.cpp` 注释错误声称等价；新增暴露 LLVM long 移位
    i32 计数未 zext 的非法定 IR。修复：掩码决策收敛到
    `codegen/jvm_semantics.hpp` 单源,RegC/LLVM 消费；新增四后端语义回归集
    （`g13.shift_out_of_range` 计数走参数防常量折叠 + `g12.string_compare`）,
    矩阵/LLVM 白名单/VMC4 E2E 全接入。
  - **X3（复核为误报）**：indy 单对象快路径 `stack[sp+1]`/`++sp` 净效应恰好
    正确（通用路径先 `sp -= argCount` 补偿）——但快路径白建 varargs 数组
    （热路径每调用多 2 次 JNI）且依赖 stale 槽。清理为参数 pop 前原槽捕获 +
    结果原位覆写,emit-shape 单测锁形态。
  - **X4（结构性）**：`staticizeStackText` 加字符串字面量/`//` 注释感知
    （indy bsm 元数据字符串内联进方法体,含 `sp -= N` 等模式会被静默改写并
    错位后续槽）；跨后端语义决策笔记沉淀 `doc_ai/cross_backend_semantics.md`
    （含 RegIR 语义显式化的后续路径）。
  - 次要：vmc4 过时"MVP 纯计算"注释与恒真死分支（误导日志）清理、新增
    `Vmc4JniSurfaceSample` protected sample（JNI helper 攻防面 + CLI round-trip
    测试）、删空 `platform.h`。
  - 验证：mj2x_unittest 全量 / mj2x_integration_test 全量（矩阵含新 fixture
    三列）/ mtest_set_unittest + mtest_set_integration_test 全绿。
- 2026-08-16（LLVM round-2 补充审查 Y1-Y4,同分支追加）：
  - **Y1**：indy 结果 REF dest 被 `unsupported type` 整方法拒绝（与注释
    "REF 直接透传"/validation 层矛盾,unboxReturnType REF 分支不可达）→ REF
    直接透传存槽;E2E 补 REF 返回站点（此前全是 (II)I 原语,零覆盖）。
  - **Y2**：bsm 失败（返回 null + pending）被送进 mj2x_llvm_call_object 的
    ThrowNew NPE 掩盖（StackC 对应路径是异常分发,移植降级）→ null 分支走
    emitExceptionDispatch;invokeWithArguments 之后统一 ExceptionCheck 再
    拆箱/透传。E2E 补恒抛 ISE 的 bsmFail 站点（BootstrapMethodError 解包
    归一化后基线/native 一致）。
  - **Y3**：indy 缓存失效路径 `br %cached_ok; ret null` → `br %resolve`
    重走解析（≈OOM 触发,意图写错）。
  - **Y4**：`Random.nextInt(bound)` 切片唯一 fail-open 角落——crt 对
    bound<=0 返回 INT32_MIN（无异常通道）而 JVM 应抛 IAE → RegC/LLVM 发射端
    预检（RegC 走 context throw + mayThrow 分发;LLVM 新增 throw_new prelude
    helper + 异常分发）,保留切片优化;RegC E2E 新用例 + LLVM 样本/E2E 覆盖。
  - 文档订正：capabilities/user_guide/dev_plan 三处"indy 运行时解析超出
    RegC"失实（regc_backend.cpp:1034 早有同功能 emitIndyBootstrapFunction）;
    native_compiler.hpp 过时"不需要 crt source pack"注释;param_registry 五个
    regc|llvm 共享参数 help 分组 RegC→RegC/LLVM;`$`→`_00024` JNI 符号
    mangling 变更与 alloca 已知限制记录入 capabilities;匹配表/逃逸分析/
    indy 解析三个共享层抽取候选补入 cross_backend_semantics.md §3.1。
- 2026-08-15：配置系统 P0+P1 完成（参数注册表 + schema/模板导出 + ConfigRegistryTest
  10 项，PR #846）。
- 更早记录已归档到 git log（见"已完成归档"索引）。

## 已完成归档索引

| 主线 | 归档位置 |
|---|---|
| 一~五（VMC4 可用版 / RegC 性能 / 异常精确化 / 反侦测 / 文档） | git log `feat/mj2x-vmc4-*`、`feat/mj2x-regc-perf`、`feat/mj2x-exc-exact`（PR #796）、`feat/mj2x-antidebug-agent`（PR #797）；`doc_ai/vmc4_multi_vm_design.md`、`doc/techniques/runtime_protection.md`、`benchmark/results/` |
| 六/七（#819 arraycopy、#820 步骤 1-5、#762/#844 变义、#843 trace surface 自验） | git log `feat/mj2x-arraycopy-intrinsic`、`feat/mj2x-vmc4-label-mutation`（PR #844）；`mj2x_integration_test` + `benchmark/baseline/regc-perf-baseline.json`；`vmc4_trace_surface_consistency_test.cpp` |
| 八~十一（#818 intrinsic、#827 program 粒度、#829 混淆） | git log PR #826；`vmc4_backend.cpp` 实例缓存 + 粒度矩阵测试；`reg_transform_passes.{hpp,cpp}` + `register_ir_test.cpp` |
| 十二（配置系统） | `mj2x-cli/src/param_registry.{hpp,cpp}`；`doc/config_file.md`；`doc_ai/config_system_design.md`（PR #846） |
| 十三（LLVM IR 后端） | `mj2x-core/src/mj2x/backend/llvmir/`（ed9a13881 实现，v2.2 转正 + 移位/互调/除零异常/递归/跨类/fptosi.sat 扩面）；`unittest/src/backend/llvm_ir_backend_test.cpp`（11 例）+ cli_test LLVM 3 例（含 clang/llc 编译 → JVM 运行 E2E）；`doc/user_guide.md` §5.4 |
| VMC4 红队对抗（N-7 / S2 前置） | `mjdbg/mjdbg-isa-recovery/`（PR #845）；`doc_ai/vmc4_red_team_contract.md` §6 |

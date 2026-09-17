# RegC 探针级性能优化：覆盖率审计 + 热点归因 + 优化点（#887 波次一 agent C，2026-09-17）

基线：`results/2026-09-17-jvm-obf-tester-os30.md`（os30 113ms vs RegC 154ms，ratio 1.36，
JDK17.0.8，单次冷启动计量）。本文档是覆盖率审计与优化点台账；after 数据由主线程
构建后回填。

## 1. 覆盖率审计表（用例 × 字节码模式 × 发射路径 × 基线耗时）

审计方法：javap 反汇编 `jvm-obf-tester.jar` 各探针/fixture，对照 RegC 基线产物
`test/jvm-obf-tester/native_project/src/generated/native.c`（26958 行）逐一核对发射形态。
RegC native 覆盖率 112/150 = 74.7%（同基线，无 skip 日志）。

| 探针 | 基线 ms | 热点字节码模式 | RegC 发射路径 | 每迭代 JNI 计数 | 结论 |
|---|---|---|---|---|---|
| Inner classes | 9.401 | 6000 迭代 ×2 个 final 类 `compute(I)` 直调；`String.hashCode`、`Class.getSimpleName` | final/no-override 直调（免门控）✅；hashCode/getSimpleName 走 cached methodID 各 1 JNI；**`ldc Class` → `_mj2x_regc_class_literal` 每次评估 1 次 FindClass（在 6000 迭代热循环内）** | ~6（含 2 GetObjectField、1 FindClass、1 GetStringLength、2 CallXxxMethodA） | FindClass 冗余 → O1 修复 |
| Crypto | 66.266 | 单次 JCA/JCE 调用（MessageDigest/Cipher），无循环 | 全部 cached static methodID + TU class cache + string literal pool，发射已最优（run() 全程 ~12 JNI，一次性） | — | 非发射问题：JDK 侧 JCA/JCE 首触初始化 + 单轮冷启动噪声（本轮实测 52.5ms vs 基线 66.3ms，自证方差） |
| Reflection | 29.561 | 1500 迭代 ×2 次 `Method.invoke` + `Integer.valueOf` 装箱 + `Object[1]` anewarray | invoke 为真实反射（1 JNI 不可省）；装箱逃逸走通用 invokestatic（1 JNI）；数组分配走 TU class cache（1 JNI）；checkcast 1 JNI | ~12 | 装箱 → O3 修复（省 2 JNI/迭代）；其余为反射语义地板 |
| Inheritance | 14.820 | 6000 迭代 ×(aaload + `AbstractWorker.dispatch` 直调 + 2 次 interface `apply`) | dispatch 免门控直调 ✅；interface 走 #780 双槽 resolved dispatch（1 IsInstanceOf + fn 直调指针）✅ 已优化形态 | ~4 | 发射已接近地板；与 os30 差距来自 os30 将热循环留在 Java 让 JIT 内联（覆盖率 61.6% vs 74.7%），非发射缺陷 |
| Enums | 8.582 | 8000 迭代 ×2 次 `values()`(clone) + name/valueOf/ordinal | 枚举 intrinsic 面（values=clone 1 JNI、name/ordinal=field 1 JNI、valueOf=1 JNI），fold 直调 | ~8 | 每步已达 1 JNI 地板；clone×2 是 javac 字节码固有语义（$VALUES 快照不可合并）。issue #887 的 7x 为 JDK21+3 轮均值口径，本轮 1 轮口径下 RegC 0.48x 反超，属测量敏感用例 |
| Exception | 0.933 | 5000 迭代 try 区 3 catch + `risky` 直调 | try 区每次调用后 1 ExceptionCheck（必要）；异常命中路径 `_mj2x_regc_exception_matches` 每匹配 **1 次 FindClass** | 常态 ~2 | FindClass 冗余 → O1 一并修复（量小） |
| String | 5.338 | 7000 迭代 charAt/codePointAt/substring×2/hashCode | 全部为 cached methodID 1-JNI 形态（capabilities 记载的刻意回退：HotSpot VM intrinsic/共享数组/O(1) substring） | ~6 | 已是文档化地板，不动 |
| Arithmetic/Control flow/Collections/Visitor/Class loading/Resource | 合计 ~24.4 | — | 纯计算 intrinsic 化 / StringBuilder CRT 链 / resolved dispatch，均已优化（#778/#780） | — | 无新低效形态 |

## 2. 热点归因清单（现象 → 根因 → 证据）

| # | 现象 | 根因 | 证据（文件:行号，基于基线产物/源码） |
|---|---|---|---|
| A1 | Inner classes 2.11x；热循环内 ldc Class 每迭代一次 FindClass | `_mj2x_regc_class_literal` 无缓存，`StaticNested.compute` 的 `getSimpleName` receiver（常量类字面量）每调用 FindClass 1 JNI | 产物 native.c:18203（循环内 class_literal）；mj2x-rt/src/regc/mj2x_regc_runtime.c 原 :597-601（直通 FindClass） |
| A2 | Exception catch 匹配路径每匹配一次 FindClass | `_mj2x_regc_exception_matches` 直通 `_mj2x_regc_find_class`（无缓存） | mj2x-rt/src/regc/mj2x_regc_runtime.c 原 :370-376；发射点 regc_flow.cpp:524 |
| A3 | 实例方法每条 Java→native 入口多 1 次 GetObjectClass，结果写入无人读取的全局 | trampoline 模板无条件发射 GetObjectClass；`_mj2x_regc_owner_class` 全库 0 读取（唯一读者是 indy bsm helper 形参/调用点实参） | 产物 native.c 122 处赋值、0 处读取（grep 证实）；发射点 regc_backend.cpp 原 :416-421；唯一引用 regc_emitter.cpp:843 |
| A4 | Reflection 装箱逃逸链每箱 1 JNI | `Integer.valueOf` 逃逸（数组存储）→ 通用 invokestatic 回退；JDK IntegerCache 本身保证 [-128,127] 对象恒一，可缓存 | 产物 native.c:24045-24051（循环内 CallStaticObjectMethodA）；emission 回退点 regc_intrinsics.cpp 原 :841-845 |
| A5 | RegC 产物高方差（60~203ms；本轮复测 Crypto 66→52ms、Inner 9.4→3.9ms） | 测量环境因素：基线为「预热 0 轮 + 1 轮计量」的冷启动单样本，JCA/JCE 首触、JIT 编译线程竞争、DLL 加载均计入探针 | results/2026-09-17-jvm-obf-tester-os30.md 头部「预热 0 轮 + 1 轮计量」；本轮复跑实测差异（52.5 vs 66.3ms）。不强行归因代码，留主线程按脚本默认 WarmupRuns=3/Runs=9 复测 |
| A6 | Crypto 2.03x | JDK 侧 JCA/JCE 首触为主（一次性行为），RegC 发射面 12 JNI 一次性不可观测 | 见 §1 Crypto 行；无发射面低效形态 |

## 3. 优化点清单（归因 → 改动 → 预期收益 → 覆盖测试）

### O1（A1+A2）：class literal / catch 类型运行时缓存
- 改动：`mj2x-rt/src/regc/mj2x_regc_runtime.c` 新增 `_mj2x_regc_cached_class_by_name`
  （64 槽 name→global-ref jclass 表，模式同 `_mj2x_intrinsic_string_literal`）；
  `_mj2x_regc_class_literal` 与 `_mj2x_regc_exception_matches` 改走缓存。
- 语义：FindClass 同 loader 同名返回同一 Class 对象（IsSameObject 恒等），缓存不改变
  任何可观测行为；TU 级 class cache 早已采用同款全局引用持有策略。
- 预期：Inner classes 每迭代省 1 FindClass（6000 次/探针）；Exception 命中路径省 FindClass。
- 覆盖测试：`RegCIntrinsicTest` ldc_class 发射形态（发射不变）+ 全部 RegC native E2E
  （异常 catch 分发、checkcast 走真 JVM）+ `mtest_set` 矩阵 G10 异常用例。

### O2（A3）：实例 trampoline GetObjectClass 条件发射
- 改动：`RegCEmitResult.usesOwnerClass`（regc_emitter.hpp）+ 文本级判定
  （regc_emitter.cpp:759-762，对最终 body 源码做子串检测，对未来任何新增引用点稳健）
  + emitNativeMethod 三分支（regc_backend.cpp:415-426）。静态方法保持 `= clazz`（无 JNI）。
- 语义：仅当生成体引用该全局（当前仅 indy 自定义 bootstrap helper 调用点）时保留
  GetObjectClass；不引用时该全局无人读取（全库 grep 证实），跳过无任何可观测差异。
- 预期：所有实例方法经 JNI 进入（resolved dispatch、Java 驱动调用）省 1 JNI/次；
  本 jar 内 trampoline 全部精简（122 处）。
- 覆盖测试：新增 `RegCBackendTest.InstanceTrampolineSkipsGetObjectClassWhenOwnerClassUnused`
  + 既有 `EmitsInstanceWrapperWithReceiverRegister`（形状不变）+ indy 阶段 3 E2E
  （usesOwnerClass=true 路径）+ 全部 native E2E。

### O3（A4）：Integer/Long/Short/Byte/Character/Boolean 逃逸装箱小值缓存
- 改动：runtime（mj2x_regc_runtime.c/.h）`_mj2x_regc_box_i32/i64` 增加小值全局引用表，
  新增 `_mj2x_regc_box_i8/i16/char/bool`；emitter（regc_intrinsics.cpp:841-871）把
  WrapperValueOf 逃逸回退改为直发缓存装箱 helper；Float/Double（无 JDK 缓存语义）保持
  原回退；`regc_flow.cpp` bodyProducesLocalRefs 增加 `_mj2x_regc_box_` marker
  （与原 `call_` 路径的帧门控行为一致）。
- 语义：命中返回对象 == JDK IntegerCache/LongCache/CharacterCache/Boolean 缓存对象
  （身份恒等）；越界值透传 JDK 调用（每次新对象）。与 JDK valueOf 逐次调用完全可观测等价。
- 预期：Reflection 循环省 2 JNI/迭代（3000 次/探针）；indy 装箱路径同享收益。
- 覆盖测试：新增 `RegCBackendTest.EscapedWrapperValueOfEmitsCachedBoxHelper` +
  `RegCSafeEnvE2E`（Integer.valueOf+hashCode 循环功能断言 w1=1225/r1=19900）+
  既有 wrapper E2E 全部 npos 断言（不含 `_mj2x_regc_call_ref(...valueOf"`）继续成立。

### O5（未做，记录为后续项）
- 枚举 `values()` clone×2：javac 固有双调用，语义上不可合并（两次快照）。
- resolved dispatch 的 1 IsInstanceOf：#780 已双槽优化，进一步省略需 SSA 级类型传播。
- String hashCode/charAt/substring 1-JNI 形态：capabilities 记载的刻意回退，CRT 版更劣。

## 4. 主线程验证步骤（精确命令）

1. 构建（xmake 由主线程执行）：
   - `xmake build mj2x-core` / `xmake build mj2x-cli` / `xmake build mj2x-rt` / `xmake build mj2x_unittest`
2. 单测：`xmake run mj2x_unittest --gtest_brief=1`
   - 重点观察：`RegCBackendTest.InstanceTrampolineSkipsGetObjectClassWhenOwnerClassUnused`（新）、
     `RegCBackendTest.EscapedWrapperValueOfEmitsCachedBoxHelper`（新）、
     `RegCBackendTest.EmitsInstanceWrapperWithReceiverRegister`、`RegCIntrinsicTest.*`、
     `RegDceTest.*`、`RegTransformPassTest.*`
3. E2E：`xmake run mj2x_integration_test`（mtest_set 20 条矩阵 + RegcPerfRegression 除外）
   与 `xmake run mj2x_runtime_test`（rt 侧改动编译验证）。
4. 基准（脚本默认即 WarmupRuns=3 / Runs=9，勿再退回单轮）：
   ```powershell
   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -SkipBuild
   ```
5. 预期观测点（机制对应）：
   - Inner classes：应有下降（每迭代 -1 FindClass，6000 次）；机制 A1/O1。
   - Reflection：应有下降（每迭代 -2 装箱 JNI，3000 次）；机制 A4/O3。
   - Exception：微降（异常命中路径省 FindClass，量小）；机制 A2/O1。
   - Inheritance/Enums/String：持平（本轮未动其路径，用于验证无回归）。
   - Crypto：看中位数是否回归到 ~30-35ms 区间（A5 噪声归因的验证：9 轮中位数应显著
     低于单轮 66ms 并接近 os30）。
   - Total 中位数预期从 154ms 向 113~130ms 区间收敛；CV% 全行 <15%。

## 5. 遗留风险

- O1：全局引用永续持有（上界 64 项）→ 类/加载器生命周期与 TU class cache 同策略，
  多 loader 共享同一 DLL 时 FindClass 首解析 loader 生效（与既有 `_mj2x_regc_resolve_verified`
  的 ownerClass 缓存一致，无新增风险面）。
- O2：文本子串判定（`_mj2x_regc_owner_class`）——未来若 runtime 新增读取该全局的
  helper（非 emitter 直引），文本扫描无法感知。缓解：mj2x-rt 为受控分发面，新增读取
  时应同步在 emitter 发射 marker；报告留档。
- O3：装箱身份语义依赖「JDK cache 对同值恒返回同一对象」（JLS 5.1.7 + JDK 实现保证，
  覆盖 Integer/Long/Short/Byte/Character/Boolean；Float/Double 未纳入）。E2E 重点盯
  装箱值 `==` 比较 / 集合去重场景（mtest_set G15 CollectionOps + safe-env E2E 数值断言）。
- 高方差归因（A5）为环境判断，若主线程 9 轮中位数下 Crypto 仍 >1.5x os30，需重新归因
  （届时怀疑面：DLL 加载时机、JNI_OnLoad 开销计入首探针）。

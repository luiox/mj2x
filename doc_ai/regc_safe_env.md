# RegC safe-env：JNI 函数表快照 + 方法入口恢复

> 状态：已实现（2026-08-10）。对应 issue #748。
> 相关代码：`mj2x-crt/src/regc/mj2x_regc_safe_env.{h,c}`、`regc_backend.cpp`
> （`emitNativeMethod` / `emitRegistrationCode`）、CLI `--regc-safe-env`。

## 1. 问题与威胁模型

RegC 编译产物（native 方法入口 + JNI 桥接）通过 `JNIEnv*` 函数表
（`JNINativeInterface_`）与 JVM 交互。分析 agent 可 hook JNI 的途径：

| 途径 | 机制 | 特征 |
|---|---|---|
| JVMTI `SetJNIFunctionTable` | 整表替换 | `env->functions` 指针变化（所有线程） |
| 原地改写表内存 | `*env->functions = hooked_copy` | **指针不变**，仅内容变化 |
| 单 entry 改写 | 表中个别函数指针替换 | 指针不变 |
| inline detour | patch jvm.dll 内函数目标首字节 | 表恢复无效（射程外） |
| JVMTI 事件层 | NativeMethodBind / MethodEntry 等 onload-only 事件 | 与表无关（射程外） |

**本方案针对前三者**。参考攻击实现：c2j-native-deobfuscator 的
`install()`（`*g_original = *g_hooked`，原地改写，指针不变）——这决定了
**内容校验是主机制**，指针比较只能作为辅助。

## 2. 设计

### 2.1 快照（capture）

`mj2x_regc_safeenv_capture(env, mode)` 在 JNI_OnLoad 首行调用（`GetEnv`
之后、任何业务逻辑之前）：

1. `g_snapshot = *env->functions`（C 模式经 `struct JNIEnv_` 转换取表）。
2. **归属校验**：每个非空表项必须位于 JVM 模块代码段
   （Windows `jvm.dll` 映像，`VirtualQuery` 的 `MEM_IMAGE` +
   `AllocationBase == jvm.dll 基址`；Linux `libjvm.so` 可执行映射区间，
   `/proc/self/maps`）。归属校验可注入替换（单测用）。
3. **豁免项**：HotSpot `UseFastJNIAccessors`（x64 默认开）把 8 个基本类型
   字段 getter（`GetBooleanField..GetDoubleField`，JNI 表索引 96..103，
   规范冻结）替换为 JIT stub，位于 JVM 代码堆（`MEM_PRIVATE` +
   `PAGE_EXECUTE`），不在 jvm.dll 映像内 → 校验豁免。
4. 校验失败（启动期 agent 已在库加载前 hook）：
   - **RESTORE 模式**：返回 `MJ2X_REGC_STATUS_INVALID_STATE`，
     JNI_OnLoad 返回 `JNI_ERR` → `System.loadLibrary` 抛
     `UnsatisfiedLinkError`（fail closed）。
   - **DEBUG 模式**：stderr 告警后返回 OK（观察用）。

### 2.2 入口恢复（restore）

`mj2x_regc_safeenv_restore(env, mode)` 在**每个 protected native 方法
JNI trampoline 的首行**（`_mj2x_regc_env = env` 之前，直接用形参 `env`）
调用：

1. 未捕获/快照无效 → 空操作。
2. `memcmp` 当前表 vs 快照；一致 → 返回（正常路径每入口一次 ~1.9KB 读）。
3. 不一致 → 逐项 8 字节原子写回（MSVC `InterlockedExchange64` /
   GCC/Clang `__atomic_store_n`），避免并发 JNI 调用读到撕裂指针。
4. DEBUG 模式打印恢复日志。

进程内所有线程的 `env->functions` 指向同一张表（HotSpot），恢复一次即
全局生效；表被指针替换（SetJNIFunctionTable）时，原子写回当前所指表的
内容同样使全部 JNI 调用恢复干净（指针指向谁无所谓，内容对即可）。

### 2.3 覆盖边界（诚实文档）

- **能防**：attach 期/运行中 agent 的表 hook（整表替换、原地改写、
  单 entry 改写）——第一次 protected 入口恢复后永久失效（一次性 install
  型 agent 无后台重装）。
- **不能防**：
  - 启动期 agent（`-agentpath`）在库加载前完成的 hook：快照必然污染，
    无干净来源可恢复 → 只能 fail closed（默认）或 debug 观察。
    这不是"短期被 hook"而是"不可恢复"，属硬边界。
  - inline detour（patch jvm.dll 函数目标）。
  - JVMTI onload-only 事件：NativeMethodBind 泄露 [C 函数地址 → Java 方法]
    映射，MethodEntry/Exit 泄露 native 调用树。safe-env 无法移除，但
    去掉 JNI 事件后 trace 只剩骨架、无数据流，recovery 基本瘫痪。
  - 校验不可用平台（非 Windows/Linux）：归属校验保守放行，仅依赖恢复。

### 2.4 开销

- 每 protected 入口：全表 memcmp（`JNINativeInterface_` ≈ 234 项 ≈ 1.9KB，
  ~100-200ns）。开关关闭时零发射、零开销。
- 恢复仅在检出时发生（罕见路径），原子写 ~1.9KB。
- 性能评估需基准数据（见 issue #748 验收节）。

## 3. 发射点与开关

| 位置 | 发射内容 | 文件 |
|---|---|---|
| JNI_OnLoad（`GetEnv` 后） | `mj2x_regc_safeenv_capture(env, MODE)`；失败 `return JNI_ERR` | `regc_backend.cpp` `emitRegistrationCode` |
| 每个 JNI trampoline 首行 | `mj2x_regc_safeenv_restore(env, MODE)` | `regc_backend.cpp` `emitNativeMethod` |
| 生成工程 include | `regc/mj2x_regc_safe_env.h`（单文件 + 多文件 api.h） | `regc_backend.cpp` finalize |
| 分发清单 | `mj2x-crt/resources.manifest` 追加 2 文件 | — |

开关（默认关闭）：
- `--regc-safe-env`：开启，RESTORE 模式（污染 fail closed）。
- `--regc-safe-env-debug`：开启 + DEBUG 模式（污染告警继续，隐含前者）。
- 均要求 `--backend regc`。
- `RegCBackend::setSafeEnv(bool)` / `setSafeEnvDebug(bool)`。

## 4. 验收

- CRT 单测（`mj2x_crt_test` 的 `test_safe_env`）：注入模块归属校验的假表，
  覆盖原地改写恢复、污染硬失败/debug 继续、未捕获空操作。
- E2E（`regc_backend_e2e_test.cpp` `RegCSafeEnvE2E`，全部走
  `ca::process::Command` 无 bat）：
  1. `EntryRestoreDefeatsInPlaceHook`：运行时原地改写（c2j 姿势），
     恢复后 JNI 调用零经过 hook（delta=0）；对照组持续被 hook（delta>0）。
  2. `HardModeFailsOnStartupPollution`：启动期污染 → 库加载失败。
  3. `DebugModeContinuesOnStartupPollution`：debug 模式告警 + 加载成功。
- 手工测试：c2j-native-deobfuscator agent（临时用，mjdbg 侧后续需一个
  类似 c2j 工具做常态化 E2E，见 roadmap）。

## 5. 后续（开放项）

- 入口 memcmp 的成本优化：白名单子集比较（生成代码实际用到的 entry）、
  抽样校验周期。
- 快照可信来源进一步加固：启动期 agent 场景的检测上报接入现有
  runtime detection policy（antijvmti 模块信号聚合）。
- mjdbg 侧落地类似 c2j 的 JNI-hook 分析工具，作为该能力的常态化
  E2E 对手（见 `doc/roadmap.md`）。

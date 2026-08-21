/// @defgroup regc_safe_env RegC safe-env JNI 函数表保护
///
/// 生成的 RegC native 代码通过 `JNIEnv*` 函数表（`JNINativeInterface_`）与 JVM
/// 交互。分析 agent（如 c2j 类工具 / JniLiveTracer）可以两种途径 hook JNI：
///
/// 1. JVMTI `SetJNIFunctionTable`：替换整表指针（`env->functions` 指向新表）；
/// 2. 原地改写表内容：`*env->functions = hooked_copy`，指针不变（c2j-native-
///    deobfuscator 的 `install()` 采用此姿势），或改写表中单个函数指针。
///
/// ## safe-env 机制
///
/// - **快照**：`mj2x_regc_safeenv_capture` 在尽可能早的时机（JNI_OnLoad 首行）
///   拷贝一份函数表，并做**归属校验**：非空表项必须指向 JVM 模块代码段
///   （Windows `jvm.dll` / Linux `libjvm.so`），唯一例外是 HotSpot
///   `UseFastJNIAccessors`（默认开）的 8 个基本类型字段 getter
///   （GetBooleanField..GetDoubleField）——它们在 VM 启动时被替换为 JIT stub，
///   位于 JVM 代码堆而非 jvm.dll 映像内，校验豁免。若捕获时表已被污染
///   （启动期 agent 早于库加载完成 hook），校验失败：
///   - 默认（RESTORE 模式）：返回 `MJ2X_REGC_STATUS_INVALID_STATE`，
///     JNI_OnLoad 据此返回 `JNI_ERR` 使库加载失败（fail closed）；
///   - DEBUG 模式：打印告警并继续（观察用，不 fail）。
/// - **入口恢复**：`mj2x_regc_safeenv_restore` 在每个 protected native 方法
///   入口首行调用。表内容与快照不一致时，用 8 字节原子写把整表内容恢复为
///   快照（指针不变时也能检出；原子写避免并发 JNI 调用读到撕裂指针）。
///   每个 Java 线程的 `env->functions` 指向同一张进程级表，恢复一次即全局生效。
///
/// ## 覆盖边界（文档化）
///
/// - 能防：`SetJNIFunctionTable` 整表替换、表内存原地改写（含单 entry 改写）。
/// - 不能防：对函数目标本身的 inline detour（表恢复后指针仍指向被 patch 的
///   jvm.dll 函数）；JVMTI 事件层（NativeMethodBind / MethodEntry 等 onload-
///   only 事件，泄露方法映射与调用树）；启动期 agent 在库加载前完成的 hook
///   ——此时快照必然污染，只能 fail closed 或 debug 观察，无法恢复干净表。
/// - 校验不可用的平台（非 Windows/Linux）：归属校验返回可接受，仅依赖恢复。

#pragma once

#include <jni.h>

#include "regc/mj2x_regc_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief safe-env 模式位。
typedef enum mj2x_safeenv_mode
{
    /// 启用入口恢复。捕获时表已污染（启动期 hook）→ capture 返回失败，
    /// JNI_OnLoad 应 fail closed（返回 JNI_ERR）。
    MJ2X_SAFEENV_MODE_RESTORE = 1,
    /// 调试观察模式：污染不 fail，打印告警后继续；恢复动作也打印日志。
    MJ2X_SAFEENV_MODE_DEBUG = 2,
} mj2x_safeenv_mode;

/// @brief 表项归属校验函数（测试可注入）。
///
/// @param addr 表中一个非空函数指针。
/// @return 非零表示地址属于受信任的 JVM 模块；零表示疑似被 hook。
typedef int (*mj2x_safeenv_module_check_fn)(const void* addr);

/// @brief 捕获并校验 JNI 函数表快照（幂等，首次生效）。
///
/// @param env 当前线程 JNIEnv。
/// @param mode MJ2X_SAFEENV_MODE_RESTORE 及其与 MJ2X_SAFEENV_MODE_DEBUG 的组合。
/// @return MJ2X_REGC_STATUS_OK 快照有效；MJ2X_REGC_STATUS_INVALID_STATE
///         表已被污染（仅非 DEBUG 模式返回，DEBUG 模式打印后仍返回 OK）；
///         MJ2X_REGC_STATUS_INVALID_ARGUMENT env 为空。
mj2x_regc_status mj2x_regc_safeenv_capture(JNIEnv* env, int mode);

/// @brief 入口恢复：表内容与快照不一致时原子恢复整表。
///
/// 快照未捕获或校验失败（污染）时为空操作。DEBUG 模式打印检出日志。
void mj2x_regc_safeenv_restore(JNIEnv* env, int mode);

/// @brief 注入表项归属校验实现（默认平台实现；测试传 NULL 恢复默认）。
void mj2x_regc_safeenv_set_module_check(mj2x_safeenv_module_check_fn fn);

#ifdef __cplusplus
}
#endif

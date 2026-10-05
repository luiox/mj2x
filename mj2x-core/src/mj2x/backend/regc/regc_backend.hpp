/// @defgroup regc_native_backend RegC Native Backend
///
/// 无栈 RegC 行的 native facade。连接共享 mj2x backend 接口到较新的
/// SSA → RegIR → RegC 管线，使 RegC 可以开始替换旧的 operand-stack 形状
/// StackC 表面。
///
/// ## 管线位置
///
/// ```text
/// MethodNode → CodeConverter L1 → SSA → RegIR → RegC helper → JNI wrapper
/// ```
///
/// RegCBackend 不直接翻译字节码。它从共享 Pipeline 接收 L1 block，
/// 构建 SSA 和 RegIR，委托 RegCEmitter 生成无栈 helper 函数，
/// 并附加 JNI 导出 wrapper。
///
/// ## 输出形状
///
/// 每个成功的方法生成两层：
/// - 静态 `regc_*` helper（使用虚拟寄存器 local，如 `r0`）
/// - 静态 `__mj2x_c_<hash>` JNI 形状 wrapper（将 JNI 参数转为 helper 参数）
///
/// RegC 用 **RegisterNatives 动态绑定**（不再是 `Java_<mangled>` 符号导出）：
/// 每个方法编译成 TU 内部的 `static` C 函数（不透明名 `__mj2x_c_<fnv1a64>`），
/// 通过 finalize 阶段发射的 RegisterNatives 表显式绑定到 Java 类。每个受保护类的
/// `<clinit>` 调 `mj2x/cloader/Loader.registerNativesForClass(classId, thisClass)`
/// 触发该类惰性注册（classloader 安全）。这彻底消除了符号名/导出的脆弱性，
/// 尤其是内部类名里的 `$` 不再需要 JNI 转义即可正确绑定。
///
/// ## 失败策略
///
/// Lowering 和 emission 是 fail-closed。如果 SSA、RegIR 或 RegC 无法处理方法，
/// 返回 `BackendResult::Kind::NONE` 且不标记 Java 方法为 native。
///
/// ## 运行时边界
///
/// 纯计算和控制流可通过生成的 JNI wrapper 执行；
/// 对象/字段/数组/monitor/类型检查操作降低为 `_mj2x_regc_*` helper 调用。

/// @file regc_backend.hpp
/// @brief Native backend facade for the Register-C pipeline.
///
/// @details
/// RegCBackend adapts the stackless RegC emitter to the shared mj2x Backend
/// interface.  It lowers one method through SSA and RegIR, emits the register
/// helper function, then wraps that helper in a RegisterNatives-bound native
/// function (opaque `static ... JNICALL __mj2x_c_<hash>`) and marks the
/// original Java method as native.  The current helper ABI is emitted
/// inline into the generated translation unit so RegC samples are executable
/// while the longer-term R3 runtime symbol/context boundary is still being
/// hardened.
#pragma once

#include "mj2x/backend/backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"

#include <memory>
#include <string>

namespace mj2x {

/// @brief Backend that emits JNI-native C using the SSA -> RegIR -> RegC path.
///
/// @details
/// The backend owns no bytecode conversion logic itself.  The shared Pipeline
/// still supplies L1 MIR blocks; RegCBackend builds SSA and RegIR from those
/// blocks, delegates stackless C generation to RegCEmitter, and appends a JNI
/// wrapper with javah-compatible symbol naming.  Methods that cannot be lowered
/// fail closed and are not injected as native methods; this prevents partially
/// translated bytecode from being replaced by a broken native stub.
class RegCBackend : public Backend
{
public:
    RegCBackend();
    ~RegCBackend() override;

    std::string_view name() const override { return "regc_native"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks,
                                CompilerIndex* index = nullptr) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    /// @brief 全输入预转换（issue #1193）：任何方法指令被清空前的唯一整输入
    /// 时机，对非库类静态方法建 L1 blocks 内联缓存，使内联被调解析与 jar
    /// 条目序/转换序解耦。--regc-no-inline 时不建缓存。
    void beforeCompile(CompilerIndex& index) override;

    /// @brief Set a `.c` output path written during finalize.
    ///
    /// @details
    /// Pipeline and E2E tests use this to compile the generated translation unit
    /// into a native library.  When empty, finalize still records
    /// `generatedSource()` but does not write a file.
    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }

    /// @brief 设置生成注释的详细级别（见 reg_codegen_syntax.md §4.2）。
    ///
    /// @details
    /// 必须在 compileMethod 之前调用：级别透传给 RegCCachePool 与 RegCEmitter，
    /// 影响函数头/块/cache 溯源注释（Summary）与逐指令 IR 注释（Detailed）。
    /// 默认 Summary。
    void setCommentLevel(RegCCommentLevel level);

    /// @brief 开启运行时调用日志（[CALL]/[DEEP]）生成。
    ///
    /// @details
    /// 开启后生成的 native 库在每次实例方法缓存调用时向 stderr 输出
    /// `[CALL] owner.name desc`，并带递归深度检测（>64 层打印 [DEEP]），
    /// 用于排查方法调用链/多线程 env 竞态问题；默认关闭，不产生任何输出。
    void setTraceCallLog(bool enabled);

    /// @brief 开启 safe-env：JNI 函数表快照 + 方法入口恢复（--regc-safe-env）。
    ///
    /// @details
    /// 开启后生成的 JNI_OnLoad 首行捕获并校验 `JNINativeInterface_` 快照
    /// （表项归属必须为 jvm.dll/libjvm.so 代码段），每个 protected native
    /// 方法入口首行恢复表内容。捕获时表已污染（启动期 agent）默认 fail
    /// closed（JNI_OnLoad 返回 JNI_ERR）；`setSafeEnvDebug(true)` 改为
    /// 打印告警后继续。默认关闭，不产生任何运行时开销。设计见
    /// `doc_ai/regc_safe_env.md`。
    void setSafeEnv(bool enabled);

    /// @brief safe-env 调试观察模式（--regc-safe-env-debug，隐含开启 safe-env）。
    ///
    /// @details
    /// 捕获污染或入口检出 hook 时只打印 stderr 告警并继续，不 fail closed；
    /// 用于开发/测试期观察，不用于交付。
    void setSafeEnvDebug(bool enabled);

    /// @brief 设置反侦测检查点注入（--runtime-protection，RegC/VMC4 通用）。
    ///
    /// @details
    /// 开启后在每个生成方法入口注入 `mj2x_rt_checkpoint_run` 调用（依赖
    /// mj2x-rt 的 rt_protect 模块，随生成工程分发）；action 见
    /// codegen::RuntimeProtectionAction。默认关闭（零产物变化）。
    void setRuntimeProtection(codegen::RuntimeProtectionAction action, std::uint32_t step,
                              std::uint32_t delayMs)
    {
        runtimeProtection_.action  = action;
        runtimeProtection_.step    = step;
        runtimeProtection_.delayMs = delayMs;
    }

    /// @brief 启用 native-to-native 直接 C 调用（默认关闭，--regc-direct-calls）。
    ///
    /// @details
    /// 关闭时所有调用走 JNI（JVM 栈帧完整，getStackTrace/StackWalker/反射语义
    /// 正确）；开启时已转换目标走 C 直调（省 JNI 开销）但栈观察语义失真，
    /// 仅用于性能优先且确认无栈观察的产物（issue #814 regc 2.6 ReTrace）。
    void setDirectCalls(bool enabled);

    /// @brief 设置多文件输出目录（--emit-native-dir）。
    ///
    /// @details
    /// 非空时 finalize 改为输出目录树：mj2x_regc_api.h（全部 extern 声明）+
    /// mj2x_regc_runtime.c（库代码：cache/helpers/indy bootstrap）+ 每类一个
    /// m_<Class>.c（用户方法体）+ mj2x_regc_entry.c（注册表/JNI_OnLoad）。
    /// 为空时保持单文件（--emit-cpp，全 static 自包含）。
    void setSplitOutputDir(const std::string& dir) { splitOutputDir_ = dir; }

    /// @brief 开启字符串字面量 at-rest 加密（--strings-encrypt，默认关闭）。
    ///
    /// @details 开启后 LDC 字面量与 indy bsm 参数字符串以密文缓冲写入 C 源
    ///          （xorshift32 密钥流逐字节 XOR，密钥基取 RegIR 变换
    ///          transformSeed——CLI 侧已按 --client-id 派生），生成代码在
    ///          JNI_OnLoad 内批量解密一次，缓冲生命周期覆盖整个执行期。默认
    ///          关闭（产物字节级不变）。必须在 compileMethod 之前调用。
    void setStringsEncrypt(bool enabled) { stringsEncrypt_ = enabled; }

    /// @brief 设置用户自定义 intrinsic（issue #818）。
    /// @details 用户条目并入 intrinsic registry（用户优先于 builtins 匹配）；
    /// 必须在 compileMethod 之前调用。
    void setUserIntrinsics(const UserIntrinsicConfig& config) override;

    /// @brief 设置 RegIR 变换选项（issue #829，Cloakwork 借鉴项）。
    ///
    /// @details 新增的 CFF 死块 / state 加密 / 深 MBA / opaque-零变体默认关闭；
    /// 必须在 compileMethod 之前调用。同 seed 同产物（可复现构建不变量）。
    void setTransformOptions(const RegTransformOptions& options) { transformOptions_ = options; }

    /// @brief 开关 SSA 方法内联（性能主线：消除 JNI 往返；默认开启）。
    ///
    /// @details 内联是语义等价的纯性能优化（静态调用 + 无异常区 + 大小/递归
    /// 门控，fail-closed）。`--regc-no-inline` 关闭，供 benchmark 对比与排查。
    void setInlineEnabled(bool enabled) { inlineEnabled_ = enabled; }

    /// @brief Full generated C translation unit after finalize.
    ///
    /// @details
    /// The source includes helper declarations/definitions, one RegC helper per
    /// successfully lowered method, and the JNI wrappers used to bind those
    /// helpers back to the protected Java class.
    const std::string& generatedSource() const override { return sourceCode_; }
    NativeSourceArtifact generatedSourceArtifact() const override
    {
        return {NativeSourceLanguage::C99, sourceCode_, "native.c"};
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string cOutputPath_;
    std::string splitOutputDir_;
    std::string sourceCode_;
    codegen::RuntimeProtectionOptions runtimeProtection_;
    RegTransformOptions               transformOptions_;
    bool                              inlineEnabled_  = true;
    bool                              stringsEncrypt_ = false;
};

}   // namespace mj2x

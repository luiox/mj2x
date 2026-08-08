#pragma once

#include "mj2x/preprocessor/preprocessor.hpp"

namespace mj2x {

/// @brief `<init>`/`<clinit>` 搬运 preprocessor（搬运法，见
/// regc_conversion_decisions.md §7.1）。
///
/// JVM 校验对构造器/类初始化器的 ACC_NATIVE 有限制（部分实现拒收 native
/// `<init>`），不能直接把 `<init>`/`<clinit>` 标记 native。改为：
/// - 生成同签名的 static 骨架 `__regc_init_<hash>` / `__regc_clinit_<hash>`，
///   把原方法体指令搬进去（label 克隆，帧交给 ClassWriter COMPUTE_FRAMES 重算）。
/// - 原 `<init>` / `<clinit>` 骨架只保留 super 调用（构造器）+ 对生成方法的调用。
/// - 生成方法名字不匹配 `<init>`，会被 MarkAll 正常标记并编译成 native。
/// - `<clinit>` 骨架保留 `registerNativesForClass` 注入点：注入发生在调用
///   生成方法之前，注册时机语义不破（§7.1）。
///
/// v1 保守范围：带 try-catch / local variable 表，或切点前含 label/frame/
/// 分支的构造器不搬运（保留字节码原样，不降级语义）。
class InitClinitMoverPreprocessor : public Preprocessor
{
public:
    void process(CompilerIndex& index) override;
};

}   // namespace mj2x
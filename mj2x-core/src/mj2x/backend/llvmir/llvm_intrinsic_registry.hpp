/// @file llvm_intrinsic_registry.hpp
/// @brief LLVM IR 后端的 JDK intrinsic 直调注册表（对齐 RegC 纯函数切片）。
#pragma once

#include "libca/str/utf8_string.hpp"

#include <string>

namespace mj2x {

/// @brief 一次 LLVM intrinsic 直调所需信息。
struct LlvmIntrinsicEntry
{
    const char* owner;   // "java/lang/Math"
    const char* name;    // "abs"
    const char* desc;    // "(I)I"
    const char* symbol;  // "mj2x_math_abs_i32"（mj2x-rt mj2x_regc_intrinsic.c 实现）
    const char* returnLlvm;   // "i32"/"i64"/"f32"/"f64"
    const char* argLlvm[2];   // 参数类型（最多 2 个；空串表示无该参数）
    const char* declareText;  // 完整 "declare i32 @sym(i32)" 文本，finalize 直接输出
};

/// @brief 查找 Math 纯函数 intrinsic（与 RegC registry 的 RuntimeFunction +
/// 纯标量条目同源：符号在 mj2x-rt，语义由 JVM 基准测试验证）。
/// @return 命中返回条目，否则 nullptr。
const LlvmIntrinsicEntry* matchLlvmIntrinsic(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc);

/// @brief 包装类装箱条目（如 Integer.valueOf(I)）。
struct LlvmWrapperBoxEntry
{
    const char* owner;       // "java/lang/Integer"
    const char* name;        // "valueOf"
    const char* desc;        // "(I)Ljava/lang/Integer;"
    const char* kind;        // "int"（shadow 结构名后缀）
    const char* shadowType;  // "%mj2x_int"
    const char* shadowDecl;  // "%mj2x_int = type { i32 }"（模块级，去重输出）
    const char* symbol;      // "mj2x_int_value_of"
    const char* argLlvm;     // "i32"（crt 形参类型；窄类型先 trunc）
    const char* declareText; // "declare %mj2x_int @mj2x_int_value_of(i32)"
};

/// @brief 包装类拆箱条目（如 Integer.intValue()）。
struct LlvmWrapperUnboxEntry
{
    const char* owner;       // "java/lang/Integer"
    const char* name;        // "intValue"
    const char* desc;        // "()I"
    const char* kind;        // "int"
    const char* shadowType;  // "%mj2x_int"
    const char* shadowDecl;  // "%mj2x_int = type { i32 }"（模块级，去重输出）
    const char* symbol;      // "mj2x_int_int_value"
    const char* retLlvm;     // "i32"（crt 返回类型；窄类型再 sext/zext）
    const char* retExt;      // "" / "sext" / "zext"（窄结果扩展到 i32）
    const char* declareText; // "declare i32 @mj2x_int_int_value(%mj2x_int)"
};

/// @brief 查找包装类 valueOf 装箱条目（仅非逃逸用法可直调；逃逸走 JNI）。
const LlvmWrapperBoxEntry* matchLlvmWrapperBox(ca::str::Utf8StringRef owner,
                                               ca::str::Utf8StringRef name,
                                               ca::str::Utf8StringRef desc);

/// @brief 查找包装类 xxxValue 拆箱条目。
const LlvmWrapperUnboxEntry* matchLlvmWrapperUnbox(ca::str::Utf8StringRef owner,
                                                   ca::str::Utf8StringRef name,
                                                   ca::str::Utf8StringRef desc);

/// @brief Random 状态机条目（shadow 结构 %mj2x_random，指针状态直调）。
///
/// @details RegC 同 face 为 inert（Allocate+<init> 融合成 NewObjectA 绕过
/// registry，方法体从不直调）；LLVM 实现真切片（对齐先例：indy 运行时解析）。
struct LlvmRandomEntry
{
    const char* owner;       // "java/util/Random"
    const char* name;        // "nextInt" / "<init>"
    const char* desc;        // "(I)I" / "(J)V"
    const char* symbol;      // "mj2x_random_next_int_bound" / "mj2x_random_init"
    const char* retLlvm;     // "i32"（void 为 "void"）
    const char* declareText; // "declare i32 @mj2x_random_next_int_bound(ptr, i32)"
};

/// @brief 查找 Random <init>(J)V（带种子构造；无参构造保持 JNI——JVM 语义
/// 依赖时钟/seedUniquifier，不确定）。
const LlvmRandomEntry* matchLlvmRandomInit(ca::str::Utf8StringRef owner,
                                           ca::str::Utf8StringRef name,
                                           ca::str::Utf8StringRef desc);

/// @brief 查找 Random 方法（nextInt/nextInt(I)/nextLong/nextBoolean/nextFloat/
/// nextDouble）。
const LlvmRandomEntry* matchLlvmRandomMethod(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc);

/// @brief StringBuilder/StringBuffer 状态机条目。
///
/// @details 状态为 crt 堆分配 `mj2x_string_builder*`（寄存器槽即 out_state
/// 指针）；全部调用返回 mj2x_regc_status（i32，非 0 = pending exception，
/// 发射端检查后分发/默认返回）。kind：0=init（out_state**），1=append
/// （返回同一 state 指针存回 dest 槽），2=method（out 参数承载结果）。
struct LlvmBuilderEntry
{
    const char* owner;       // "java/lang/StringBuilder" / "java/lang/StringBuffer"
    const char* name;        // "<init>" / "append" / "toString" / ...
    const char* desc;        // "(I)V" / "(I)Ljava/lang/StringBuilder;" / ...
    const char* symbol;      // "mj2x_string_builder_append_i32"
    int         kind;        // 0=init 1=append 2=method
    const char* outLlvm;     // method 的 out 参数类型："" / "ptr" / "i32" / "i16"
    const char* argLlvm[3];  // context/state 之后的参数（i32/i64/float/double/ptr；
                             // i8/i16 表示需 trunc——crt 形参为 int8_t/uint16_t）
};

/// @brief 查找 StringBuilder/StringBuffer 状态机条目（init/append/method）。
const LlvmBuilderEntry* matchLlvmBuilder(ca::str::Utf8StringRef owner,
                                         ca::str::Utf8StringRef name,
                                         ca::str::Utf8StringRef desc);

/// @brief 判断 Allocate 的 typeDesc 是否 StringBuilder/StringBuffer。
bool isLlvmBuilderAllocation(ca::str::Utf8StringRef typeDesc);

}   // namespace mj2x

/// @defgroup string_intrinsic String/MutableString intrinsic 契约
///
/// 建立 RegC 运行时边界的 String/StringBuilder/StringBuffer intrinsic 集。
/// 实现在 `mj2x-crt` 中拥有 UTF-16 状态，在逃逸点物化为真实 Java 对象，
/// 当可变链无法在不发散的情况下保持在 native 中时回退到 Java。
///
/// ## 现有运行时切片
///
/// `mj2x-runtime/src/intrinsic/intrinsic_string.hpp` 定义：
///
/// - `mj2x_utf16_view`：runtime-owned UTF-16 code-unit 视图
/// - `mj2x_string_status`：显式 fail-closed 状态码
/// - `mj2x_string_length`：最小 `String.length()` 语义
/// - `mj2x_string_char_at`：最小 `String.charAt(int)` 语义
/// - `mj2x_stringbuilder_unsupported_status`：StringBuilder/StringBuffer 失败标记
///
/// 运行时切片不接受 `jstring` 也不返回 `jobject`。JNI 字符串物化保持在 emitter
/// 本地 helper 中。
///
/// ## 可分发可变 String C ABI
///
/// `mj2x-crt/src/regc/mj2x_regc_crt.h` 是公共 ABI，保持 `mj2x_string_builder`
/// 和兼容的 `mj2x_string_buffer` 存储不透明，仅暴露 C99/JNI 操作：
/// 显式默认/String/CharSequence/capacity 初始化 + destroy 生命周期、
/// UTF-16 code-unit append + jstring/对象/基本类型/char[]/范围/code-point append、
/// 可观察 length/capacity、ensure/trim、setLength、charAt、setCharAt、
/// to_jstring/materialize 桥接、显式状态码与异常桥接。
///
/// `mj2x_string_buffer_*` 使用递归 CRITICAL_SECTION/pthread mutex。
///
/// ## Emitter 策略
///
/// - UTF-16 存储由 generated/runtime 代码拥有，存活超过调用
/// - 逃逸值在用作 jobject/字段/数组元素/未知调用参数前物化
/// - 物化后变异的链从其分配点回退到 Java
/// - 不支持的操作 fail-closed，不清不楚
///
/// ## 验证覆盖
///
/// `mj2x_runtime_test` 覆盖 UTF-16 length/charAt、空/null 视图、越界 fail-closed、
/// StringBuilder 不支持状态。`mj2x_crt_test` 覆盖 CRT mutable String ABI 的
/// append/length/capacity、分配失败报告、异常桥接、并发 StringBuffer append。
/// Native E2E 比较 range/capacity、escape-aware materialization、文字/regex split
/// 和非法 regex 行为与 JDK 17 的一致性。

/**
 * @file intrinsic_string.hpp
 * @brief String/StringBuilder intrinsic 的最小 UTF-16 垫片。
 *
 * @details
 * 这个头只定义 RegC 未来 String intrinsic 可以消费的 runtime-owned UTF-16
 * view 和 fail-closed 状态。它不表示 Java `jstring`,不做 GC,也不尝试实现
 * `StringBuilder` 的可变对象语义。emitter 只有在能证明字符串数据不逃逸且由
 * runtime 拥有时才允许调用这些函数。
 */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

/// @brief String intrinsic 的返回状态。
enum mj2x_string_status : std::int32_t
{
    MJ2X_STRING_OK                          = 0,
    MJ2X_STRING_UNSUPPORTED_NULL            = -1,
    MJ2X_STRING_UNSUPPORTED_BOUNDS          = -2,
    MJ2X_STRING_UNSUPPORTED_ESCAPE          = -3,
    MJ2X_STRING_UNSUPPORTED_MUTABLE_BUILDER = -4,
};

/// @brief runtime-owned UTF-16 字符串视图;调用方负责保证底层内存存活。
struct mj2x_utf16_view
{
    const std::uint16_t* units;
    std::size_t          length;
};

/**
 * @brief 创建 UTF-16 view。
 *
 * @param units UTF-16 code unit 起始地址;长度为 0 时允许为空。
 * @param length code unit 数量。
 * @return 可传给后续 String intrinsic 的 view。
 */
mj2x_utf16_view mj2x_utf16_view_of(const std::uint16_t* units, std::size_t length);

/**
 * @brief `String.length()` 的最小 native 语义。
 *
 * @param view runtime-owned UTF-16 view。
 * @param status 可为空;非空时写入状态。
 * @return 成功时返回 code unit 长度;失败时返回 0。
 */
std::int32_t mj2x_string_length(mj2x_utf16_view view, mj2x_string_status* status);

/**
 * @brief `String.charAt(int)` 的最小 native 语义。
 *
 * @param view runtime-owned UTF-16 view。
 * @param index code unit 下标。
 * @param status 可为空;非空时写入状态。
 * @return 成功时返回 UTF-16 code unit;失败时返回 0。
 */
std::uint16_t mj2x_string_char_at(mj2x_utf16_view view, std::size_t index,
                                  mj2x_string_status* status);

/**
 * @brief StringBuilder/StringBuffer 当前统一 fail-closed 的状态码。
 *
 * @details 可变字符串涉及容量增长、逃逸、同步和对象物化,当前 R3 切片不实现。
 */
mj2x_string_status mj2x_stringbuilder_unsupported_status();

}   // extern "C"

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

/// @file user_intrinsics.hpp
/// @brief 用户自定义 intrinsic 配置（issue #818）。
///
/// @details
/// 任何后端（StackC / RegC / VMC4）都允许用户通过 CLI 传入自定义 intrinsic：
/// 把某个 Java 调用点（owner.name:desc）降级为对用户提供的 native symbol 的
/// 直接调用，而不是通用 JNI 派发。配置 JSON 由 CLI 解析（nlohmann/json，
/// 见 mj2x-cli main.cpp），core 只消费本文件的结构。字段用 std::string
/// （配置边界数据，可拷贝赋值，避免 Utf8String 的不可赋值约束）。
#pragma once

#include <string>
#include <vector>

namespace mj2x {

/// @brief 一条用户 intrinsic：Java 调用点 -> native symbol。
///
/// @details
/// - `owner` 用内部形式（`java/lang/Math`，不带 `L...;`）。
/// - `desc` 是 JVM 方法描述符（`(II)I`）。
/// - `symbol` 是用户 native 实现的可链接符号名；实现文件经
///   UserIntrinsicConfig::sources 并入生成工程编译。
/// - `passContext`：为 true 时在参数前传 `JNIEnv* env`（三后端一致；
///   RegC 传 `_mj2x_regc_current_context()->env`）。
/// - `mayThrow`：为 true 时调用点保留调用后 ExceptionCheck（StackC）。
///   RegC 的 UserIntrinsic 恒保留检查（不做纯计算豁免）。
struct UserIntrinsic
{
    std::string owner;
    std::string name;
    std::string desc;
    std::string symbol;
    bool        passContext = false;
    bool        mayThrow    = false;
};

/// @brief 用户 intrinsic 全集：条目 + 用户 native 实现源文件。
struct UserIntrinsicConfig
{
    std::vector<UserIntrinsic> intrinsics;
    /// 用户 native 实现文件（.c/.cpp），复制进生成工程并参与编译
    /// （--intrinsic-config 的 "sources" 数组与 --intrinsic-src 合并）。
    std::vector<std::string> sources;
};

}   // namespace mj2x

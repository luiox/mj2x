#pragma once

// Shared RegisterNatives bridge constants.
//
// mj2x/cloader/Loader 是运行时桥接类（由 CLoaderPostprocessor 注入），其 native
// 方法 `registerNativesForClass(ILjava/lang/Class;)V` 由本侧 C 函数
// `_mj2x_register_for_class` 实现，在 JNI_OnLoad 时用 RegisterNatives 绑定到
// Loader 类上。每个受保护类的 <clinit> 调用该方法触发该类的惰性 RegisterNatives。
//
// 这些常量此前只在 StackC 内部各写一份，现提取为共享头，供 RegC 复用同一桥接机制。
// 配套的 clinit 字节码注入参照 StackC 的 emitClassRegistrationCall（stackc_backend.cpp）。

namespace mj2x {
namespace cloader {

/// 桥接类的内部类名。
inline constexpr const char* kClassName = "mj2x/cloader/Loader";

/// 桥接方法名：registerNativesForClass(int classId, Class clazz)。
inline constexpr const char* kRegisterName = "registerNativesForClass";

/// 桥接方法描述符。
inline constexpr const char* kRegisterDesc = "(ILjava/lang/Class;)V";

/// 实现桥接方法的 C 函数符号（JNI_OnLoad 时绑定）。
inline constexpr const char* kBridgeCFunc = "_mj2x_register_for_class";

}   // namespace cloader
}   // namespace mj2x

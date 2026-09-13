#pragma once

// Shared native-loader bytecode emission.
//
// mj2x/runtime/NativeLoader 是运行时从 JAR 资源加载 native 库的引导类，
// clinit 里调用其 `load(String resourcePath, String tempSuffix)` 完成加载。
// 该调用此前在 RegC / StackC / NativeLoaderPostprocessor 三处各写一份常量
// 和指令序列，本文件收口为单一来源。
//
// StackC 因 RegisterNatives 绑定需要在自己的 protectClassInitializer 里
// 重写整个 clinit，故仅复用本文件的常量与 load 指令发射；RegC 的 clinit
// 前插逻辑则由 NativeLoaderPostprocessor 调用 prependNativeLoaderCall 完成。

#include <jbytecode/jbytecode.hpp>

#include <algorithm>
#include <string>

namespace mj2x {
namespace native_loader {

/// NativeLoader 的内部类名。
inline constexpr const char* kClassName = "mj2x/runtime/NativeLoader";

/// NativeLoader.load 的描述符：(Ljava/lang/String;Ljava/lang/String;)V。
inline constexpr const char* kLoadDesc = "(Ljava/lang/String;Ljava/lang/String;)V";

/// @brief 往 clinit 指令列表头部插入 NativeLoader.load(resourcePath, tempSuffix) 调用。
///
/// @details 发射 3 条指令（LDC path / LDC suffix / INVOKESTATIC load），作为一个
/// 连续前缀块整体插入到 clinit 现有指令之前，并把 maxStack 提升到至少 2。
/// resourcePath 为空或 clinit 为空时静默返回。
inline void emitLoadCall(jbytecode::tree::MethodNode* clinit, const std::string& resourcePath,
                         const std::string& tempSuffix)
{
    if (resourcePath.empty() || clinit == nullptr) return;
    using namespace jbytecode::core;
    using namespace jbytecode::tree;
    InsnList prefix;
    prefix.add(clinit->create<LdcInsnNode>(resourcePath));
    prefix.add(clinit->create<LdcInsnNode>(tempSuffix));
    prefix.add(clinit->create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                                              ca::str::ZUtf8StringRef::from_static(kClassName),
                                              ca::str::ZUtf8StringRef::from_static("load"),
                                              ca::str::ZUtf8StringRef::from_static(kLoadDesc),
                                              false));
    clinit->instructions.insert(clinit->instructions.getFirst(), prefix);
    clinit->maxStack = (std::max)(clinit->maxStack, 2);
}

}   // namespace native_loader
}   // namespace mj2x

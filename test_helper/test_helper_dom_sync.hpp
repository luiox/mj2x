#pragma once

/// @file        test_helper_dom_sync.hpp
/// @brief       批 6d 后的测试三明治：树夹具 ↔ libjbc DOM 权威。
///
/// CompilerClass 树载体退役（只剩 dom + 元数据快照）后，产品侧同步对
/// syncTreeToDom/syncDomToTree 一并消失。断言面保持树形态的路径改为两步制：
///   建模 = classFromTree（树 → plain 字节 → DOM + 快照对齐）；
///   断言 = treeFromDom（DOM → writeClass 字节 → ClassReader 重建树）。
/// 序列化旗标恒 plain（0），与退役的 class_dom_sync 同口径——帧重算归 output
/// 侧语义，本层只求「如实序列化当前状态」。
///
/// 命名说明：进 namespace test（与 test_helper.hpp 同域）——大量测试 TU 同时
/// `using namespace mj2x;` + `test::foo()` 调用，mj2x::test 会在限定查找里与
/// ::test 撞名（两个不同 namespace 同名 = 歧义）。

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/frontend/code_converter.hpp"

namespace test {

/// @brief 树夹具 → CompilerClass（DOM 权威 + 元数据快照）。
///
/// ClassWriter(plain 0) 序列化 → jbc::core::readClass → dom 挂入 →
/// refreshMethodSnapshots 对齐快照。@p node 只读借视图；返回值 move 进 index。
mj2x::CompilerClass classFromTree(const std::string& name, jbytecode::tree::ClassNode& node,
                                  ca::str::Utf8StringArena& arena);

/// @brief DOM → 树回投影（断言面保持树形态）。
///
/// writeClass 保真字节 → 旧栈 ClassReader 重建树。返回 unique_ptr：树方法指针
/// 仅在本树存活期内有效，断言面请先持树再取方法指针。
std::unique_ptr<jbytecode::tree::ClassNode> treeFromDom(const mj2x::CompilerClass& cls,
                                                        ca::str::Utf8StringArena& arena);

/// @brief 树方法现查（按名；node == nullptr 时安全返回 nullptr）。
jbytecode::tree::MethodNode* findMethodNode(jbytecode::tree::ClassNode* node,
                                            const std::string&          name);

/// @brief 独立方法快照（无类夹具的后端单测用）。
///
/// name/desc/access/maxStack/maxLocals 自树方法取（DOM 对齐语义同
/// refreshMethodSnapshots），管线旗标默认；@p markedForCompile 一步置位。
mj2x::CompilerMethod snapshotMethod(jbytecode::tree::MethodNode& mn,
                                    bool                         markedForCompile = false);

/// @brief CodeConverter 测试垫片：独立 MethodNode → convert(cf, 0)。
///
/// 方法挂最小包装类（shim/Class，plain 序列化 → readClass → DOM 第 0 方法），
/// 与批 6d 退役的 CodeConverter::convert(MethodNode&) 同一读路径。converter
/// 构造注入的 arena 即 @p arena（Block 内字符串入池）。
std::vector<mj2x::Block*> convertMethodNode(mj2x::CodeConverter&         converter,
                                            jbytecode::tree::MethodNode& mn,
                                            ca::str::Utf8StringArena&    arena);

}   // namespace test

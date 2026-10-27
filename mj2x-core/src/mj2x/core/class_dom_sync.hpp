/// @file        class_dom_sync.hpp
/// @brief       tree ↔ libjbc DOM 阶段边界同步（libjbc 迁移批 2）。
///
/// 双表示过渡期（批 2-5）的唯一同步通道：**真实编解码器字节往返**，不写
/// 模型间适配层——tree → ClassWriter 字节 → `jbc::core::readClass` → DOM，
/// 反向 DOM → `writeClass` 字节 → ClassReader → tree。同步点钉在 pipeline
/// 阶段边界（runPreprocessors 后 / compileMethods 后），各批的新鲜度契约：
///
/// - 批 2：DOM 只读影子（无消费方），方向恒 tree→DOM；
/// - 批 3：frontend/backend 读 DOM，预处理/注入仍改 tree（既有同步点覆盖）；
/// - 批 4：pre/post 改 DOM，post 后需 DOM→tree（output 仍读树）；
/// - 批 5：注入改 DOM，编译后同步点消失；
/// - 批 6：output 改读 DOM，tree 载体与同步器一并退役。
///
/// 序列化旗标与 output_writer 同口径（modified → COMPUTE_FRAMES，否则
/// COMPUTE_MAXS），保证 DOM 反映的字节形态与最终产物一致。
#pragma once

#include "mj2x/core/compiler_index.hpp"

namespace mj2x {

/// @brief 对 index 内全部类执行 tree→DOM 同步（重建 dom，含未修改类）。
///
/// Err 即抛 std::runtime_error（pipeline 边界既有 catch 面）。
void syncTreeToDom(CompilerIndex& index);

/// @brief 对 index 内全部类执行 DOM→tree 反向同步（重建 classNode/methods/fields）。
///
/// 批 4 载体切换后 DOM 是改写权威：DOM → `writeClass` 字节 → 旧栈
/// ClassReader → 新 tree。CompilerMethod 的管线状态（markedForCompile/
/// compiled/backendResult）按 name+desc（类内唯一）锚定迁移到新节点，
/// DOM 侧新增的方法（如 __regc_* 搬运产物）按默认状态落位。
/// Err 即抛 std::runtime_error（pipeline 边界既有 catch 面）。
void syncDomToTree(CompilerIndex& index);

}   // namespace mj2x

#include "invokedynamic_preprocessor.hpp"

#include "libca/core/any.hpp"
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

void InvokeDynamicPreprocessor::process(CompilerIndex& index) {
    // R1b：检测 invokedynamic，明确标记为不支持（不静默落 MIRNop）。
    //
    // 完整展开（bootstrap 调用 + MethodHandle.invokeWithArguments + 装箱/拆箱 + try-catch）
    // 需要 L5 的装箱/拆箱 lowering 基础设施，R1b 阶段尚未具备。故 R1b 的策略是：
    //   - 扫描每个方法，若有 invokedynamic，打印明确告警
    //   - 不改写字节码（保留 invokedynamic）
    //   - CodeConverter 遇到 invokedynamic 时会落 MIRInvokeDynamic（fail-closed 在 builder）
    //
    // 真正的字节码展开（doc/PREPROCESSOR.md §4 的 STD_JAVA 路径）推到 R2/R5+：
    //   - R2：当 RegC 有了装箱/拆箱 lowering 后，移植 native-obfuscator IndyPreprocessor
    //   - R5+：若 SnakeGame 触发（lambda / String.concat），优先实现
    //
    // 这样 R1b 至少消除了"静默落 MIRNop"的隐患（invokedynamic 不再被忽略）。
    int totalIndy = 0;
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto* m : cls.classNode->methods) {
            for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
                if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
                    ++totalIndy;
                }
            }
        }
    }
    if (totalIndy > 0) {
        printf("[InvokeDynamicPreproc] Detected %d invokedynamic site(s). "
               "Full bytecode expansion deferred to R2 (needs boxing/unboxing lowering). "
               "Sites will fail-closed in SSA builder, not silently dropped.\n",
               totalIndy);
    }
}

} // namespace mj2x

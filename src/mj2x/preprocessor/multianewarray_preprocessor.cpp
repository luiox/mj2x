#include "multianewarray_preprocessor.hpp"

#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

void MultiANewArrayPreprocessor::process(CompilerIndex& index) {
    // R1b（方案 A）：检测 multianewarray 并告警。不改字节码。
    // builder 遇到 MIRNewArray(dims>1) 时 fail-closed，不静默。
    // 真正的字节码层循环展开（方案 B）推到 R2+，SnakeGame 无 multianewarray，不阻塞。
    int total = 0;
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto* m : cls.classNode->methods) {
            for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
                if (insn->getType() == AbstractInsnNode::MULTIANEWARRAY_INSN) {
                    ++total;
                }
            }
        }
    }
    if (total > 0) {
        printf("[MultiANewArrayPreproc] Detected %d multianewarray site(s). "
               "Bytecode expansion deferred to R2+. Sites will fail-closed in builder.\n",
               total);
    }
}

} // namespace mj2x

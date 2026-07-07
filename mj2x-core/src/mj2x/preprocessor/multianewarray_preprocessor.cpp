#include "multianewarray_preprocessor.hpp"

#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

void MultiANewArrayPreprocessor::process(CompilerIndex& index) {
    // Keep this preprocessor observational.  CodeConverter and RegC already
    // preserve MULTIANEWARRAY as a helper-backed operation, so bytecode-level
    // loop expansion would only make the input harder to reason about.
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
               "Keeping bytecode shape; RegC lowers supported sites through its multi-array helper.\n",
               total);
    }
}

} // namespace mj2x

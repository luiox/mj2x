#include "invokedynamic_preprocessor.hpp"

#include "libca/core/any.hpp"
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

void InvokeDynamicPreprocessor::process(CompilerIndex& index)
{
    // StackC 在 MIR 层保留 bootstrap 元数据，并在 native 侧走
    // CallSite.getTarget() + MethodHandle.invokeWithArguments。RegC/SSA 仍要求
    // indy 在进入无栈 IR 前被展开，因此这里不改写字节码，只做可见诊断。
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
               "StackC will lower them through a runtime MethodHandle bridge; "
               "RegC/SSA still fail-closed until bytecode/IR expansion exists.\n",
               totalIndy);
    }
}

}   // namespace mj2x

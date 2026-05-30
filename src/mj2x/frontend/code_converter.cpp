#include "code_converter.hpp"
#include "opcode_utils.hpp"
#include "type_utils.hpp"

#include <cassert>
#include <sstream>
#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

struct CodeConverter::Impl {
    std::unordered_map<int, std::unique_ptr<Block>> blocks;
    std::vector<Block*> blockOrder;
    Block* currentBlock = nullptr;
    std::unordered_set<int> labelTargets;
    std::vector<ExceptionHandler> handlers;
};

CodeConverter::CodeConverter()
    : impl_(std::make_unique<Impl>())
{}

CodeConverter::~CodeConverter() = default;

Block* CodeConverter::getOrCreateBlock(int labelId) {
    auto it = impl_->blocks.find(labelId);
    if (it != impl_->blocks.end())
        return it->second.get();
    auto block = std::make_unique<Block>(labelId);
    auto* ptr = block.get();
    impl_->blocks[labelId] = std::move(block);
    impl_->blockOrder.push_back(ptr);
    return ptr;
}

void CodeConverter::analyzeControlFlow(MethodNode& mn) {
    impl_->labelTargets.clear();
    impl_->handlers.clear();

    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn) {
        switch (insn->getType()) {
        case AbstractInsnNode::JUMP_INSN: {
            auto* jump = ca::core::cast<JumpInsnNode>(insn);
            impl_->labelTargets.insert(jump->label->bytecodeOffset);
            break;
        }
        case AbstractInsnNode::TABLESWITCH_INSN: {
            auto* ts = ca::core::cast<TableSwitchInsnNode>(insn);
            impl_->labelTargets.insert(ts->dflt->bytecodeOffset);
            for (auto* l : ts->labels)
                impl_->labelTargets.insert(l->bytecodeOffset);
            break;
        }
        case AbstractInsnNode::LOOKUPSWITCH_INSN: {
            auto* ls = ca::core::cast<LookupSwitchInsnNode>(insn);
            impl_->labelTargets.insert(ls->dflt->bytecodeOffset);
            for (auto* l : ls->labels)
                impl_->labelTargets.insert(l->bytecodeOffset);
            break;
        }
        default:
            break;
        }
    }

    for (auto* tcb : mn.tryCatchBlocks) {
        impl_->labelTargets.insert(tcb->handler->bytecodeOffset);
        ExceptionHandler h;
        h.startLabel = tcb->start->bytecodeOffset;
        h.endLabel = tcb->end->bytecodeOffset;
        h.handlerLabel = tcb->handler->bytecodeOffset;
        h.catchType = tcb->type;
        impl_->handlers.push_back(h);
    }

    // Entry point
    impl_->labelTargets.insert(0);
}

void CodeConverter::buildBlocks() {
    impl_->blocks.clear();
    impl_->blockOrder.clear();
    for (auto id : impl_->labelTargets)
        getOrCreateBlock(id);
}

void CodeConverter::processInsn(AbstractInsnNode* insn) {
    auto& cur = impl_->currentBlock;
    if (!cur) {
        cur = impl_->blockOrder.empty() ? getOrCreateBlock(0) : impl_->blockOrder[0];
    }

    switch (insn->getType()) {
    case AbstractInsnNode::LABEL: {
        auto* lb = ca::core::cast<LabelNode>(insn);
        int lid = lb->label->bytecodeOffset;
        auto it = impl_->blocks.find(lid);
        if (it != impl_->blocks.end())
            cur = it->second.get();
        cur->nodes.push_back(std::make_unique<MIRLabel>(lid));
        break;
    }

    case AbstractInsnNode::INSN: {
        int opc = insn->opcode;
        if (opc == Opcodes::NOP) {
            cur->nodes.push_back(std::make_unique<MIRNop>());
        } else if (opc == Opcodes::ACONST_NULL) {
            cur->nodes.push_back(std::make_unique<MIRConstant>());
        } else if (opc >= Opcodes::ICONST_M1 && opc <= Opcodes::DCONST_1) {
            cur->nodes.push_back(std::make_unique<MIRConstant>());
        } else if (opc >= Opcodes::IALOAD && opc <= Opcodes::SALOAD) {
            cur->nodes.push_back(std::make_unique<MIRArrayLoad>());
        } else if (opc >= Opcodes::IASTORE && opc <= Opcodes::SASTORE) {
            cur->nodes.push_back(std::make_unique<MIRArrayStore>());
        } else if (opc == Opcodes::POP || opc == Opcodes::POP2) {
            cur->nodes.push_back(std::make_unique<MIRNop>());
        } else if (opc == Opcodes::DUP) {
            cur->nodes.push_back(std::make_unique<MIRStackOp>(MIRKind::STACK_DUP));
        } else if (opc == Opcodes::DUP_X1) {
            cur->nodes.push_back(std::make_unique<MIRStackOp>(MIRKind::STACK_DUP_X1));
        } else if (opc == Opcodes::DUP_X2) {
            cur->nodes.push_back(std::make_unique<MIRStackOp>(MIRKind::STACK_DUP_X2));
        } else if (opc == Opcodes::DUP2) {
            cur->nodes.push_back(std::make_unique<MIRStackOp>(MIRKind::STACK_DUP2));
        } else if (opc == Opcodes::SWAP) {
            cur->nodes.push_back(std::make_unique<MIRStackOp>(MIRKind::STACK_SWAP));
        } else if ((opc >= Opcodes::IADD && opc <= Opcodes::DREM) ||
                   (opc >= Opcodes::ISHL && opc <= Opcodes::LXOR)) {
            auto node = std::make_unique<MIRBinaryOp>();
            node->op = opcodeToBinaryOp(opc);
            node->type = opcodeToResultType(opc);
            cur->nodes.push_back(std::move(node));
        } else if (opc >= Opcodes::INEG && opc <= Opcodes::DNEG) {
            cur->nodes.push_back(std::make_unique<MIRUnaryOp>());
        } else if ((opc >= Opcodes::I2L && opc <= Opcodes::D2I) ||
                   opc == Opcodes::I2B || opc == Opcodes::I2C || opc == Opcodes::I2S) {
            cur->nodes.push_back(std::make_unique<MIRCast>());
        } else if (opc == Opcodes::LCMP || opc == Opcodes::FCMPL ||
                   opc == Opcodes::FCMPG || opc == Opcodes::DCMPL ||
                   opc == Opcodes::DCMPG) {
            cur->nodes.push_back(std::make_unique<MIRBinaryOp>());
        } else if (opc >= Opcodes::IRETURN && opc <= Opcodes::ARETURN) {
            cur->nodes.push_back(std::make_unique<MIRReturnValue>());
        } else if (opc == Opcodes::RETURN) {
            cur->nodes.push_back(std::make_unique<MIRReturn>());
        } else if (opc == Opcodes::ARRAYLENGTH) {
            cur->nodes.push_back(std::make_unique<MIRArrayLength>());
        } else if (opc == Opcodes::ATHROW) {
            cur->nodes.push_back(std::make_unique<MIRThrow>());
        } else if (opc == Opcodes::MONITORENTER) {
            cur->nodes.push_back(std::make_unique<MIRMonitor>()); // enter=true default
        } else if (opc == Opcodes::MONITOREXIT) {
            auto node = std::make_unique<MIRMonitor>();
            node->enter = false;
            cur->nodes.push_back(std::move(node));
        }
        break;
    }

    case AbstractInsnNode::INT_INSN: {
        auto* ii = ca::core::cast<IntInsnNode>(insn);
        if (ii->opcode == Opcodes::BIPUSH || ii->opcode == Opcodes::SIPUSH) {
            cur->nodes.push_back(std::make_unique<MIRConstant>());
        } else if (ii->opcode == Opcodes::NEWARRAY) {
            auto node = std::make_unique<MIRNewArray>();
            (void)ii; // type info available via ii->operand
            cur->nodes.push_back(std::move(node));
        }
        break;
    }

    case AbstractInsnNode::VAR_INSN: {
        auto* vi = ca::core::cast<VarInsnNode>(insn);
        int opc = vi->opcode;
        if (opc >= Opcodes::ILOAD && opc <= Opcodes::ALOAD) {
            auto node = std::make_unique<MIRLocalLoad>();
            node->varIndex = vi->var;
            cur->nodes.push_back(std::move(node));
        } else if (opc >= Opcodes::ISTORE && opc <= Opcodes::ASTORE) {
            auto node = std::make_unique<MIRLocalStore>();
            node->varIndex = vi->var;
            cur->nodes.push_back(std::move(node));
        }
        // RET handled in preprocessor
        break;
    }

    case AbstractInsnNode::TYPE_INSN: {
        auto* ti = ca::core::cast<TypeInsnNode>(insn);
        switch (ti->opcode) {
        case Opcodes::NEW: {
            auto node = std::make_unique<MIRNewObject>();
            node->typeDesc = ti->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::ANEWARRAY: {
            auto node = std::make_unique<MIRNewArray>();
            node->typeDesc = ti->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::CHECKCAST: {
            auto node = std::make_unique<MIRCheckCast>();
            node->typeDesc = ti->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::INSTANCEOF: {
            auto node = std::make_unique<MIRInstanceOf>();
            node->typeDesc = ti->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        default:
            break;
        }
        break;
    }

    case AbstractInsnNode::FIELD_INSN: {
        auto* fi = ca::core::cast<FieldInsnNode>(insn);
        switch (fi->opcode) {
        case Opcodes::GETSTATIC: {
            auto node = std::make_unique<MIRStaticGet>();
            node->owner = fi->owner; node->name = fi->name; node->desc = fi->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTSTATIC: {
            auto node = std::make_unique<MIRStaticPut>();
            node->owner = fi->owner; node->name = fi->name; node->desc = fi->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::GETFIELD: {
            auto node = std::make_unique<MIRFieldGet>();
            node->owner = fi->owner; node->name = fi->name; node->desc = fi->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTFIELD: {
            auto node = std::make_unique<MIRFieldPut>();
            node->owner = fi->owner; node->name = fi->name; node->desc = fi->desc;
            cur->nodes.push_back(std::move(node));
            break;
        }
        default:
            break;
        }
        break;
    }

    case AbstractInsnNode::METHOD_INSN: {
        auto* mi = ca::core::cast<MethodInsnNode>(insn);
        auto node = std::make_unique<MIRInvoke>();
        node->owner = mi->owner;
        node->name = mi->name;
        node->desc = mi->desc;
        node->isInterface = mi->isInterface;
        node->invokeOpcode = mi->opcode;
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::INVOKE_DYNAMIC_INSN: {
        // Preprocessor extracts these; treat as NOP for now
        cur->nodes.push_back(std::make_unique<MIRNop>());
        break;
    }

    case AbstractInsnNode::JUMP_INSN: {
        auto* ji = ca::core::cast<JumpInsnNode>(insn);
        int opc = ji->opcode;
        int target = ji->label->bytecodeOffset;

        if (opc == Opcodes::GOTO || opc == Opcodes::GOTO_W) {
            auto node = std::make_unique<MIRUncondBranch>();
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        } else if (opc == Opcodes::IF_ACMPEQ || opc == Opcodes::IF_ACMPNE) {
            auto node = std::make_unique<MIRBranch>();
            node->op = branchOpcodeToCmpOp(opc);
            node->type = PrimitiveKind::OBJECT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        } else if (opc >= Opcodes::IF_ICMPEQ && opc <= Opcodes::IF_ICMPLE) {
            auto node = std::make_unique<MIRBranch>();
            node->op = branchOpcodeToCmpOp(opc);
            node->type = PrimitiveKind::INT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        } else if (opc == Opcodes::IFNULL || opc == Opcodes::IFNONNULL ||
                   (opc >= Opcodes::IFEQ && opc <= Opcodes::IFLE)) {
            auto node = std::make_unique<MIRBranchZero>();
            node->op = branchZeroOpcodeToCmpOp(opc);
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        } else {
            // JSR/JSR_W — preprocessor extracts
            cur->nodes.push_back(std::make_unique<MIRNop>());
        }
        break;
    }

    case AbstractInsnNode::LDC_INSN: {
        cur->nodes.push_back(std::make_unique<MIRConstant>());
        break;
    }

    case AbstractInsnNode::IINC_INSN: {
        auto* ii = ca::core::cast<IincInsnNode>(insn);
        auto node = std::make_unique<MIRIinc>();
        node->varIndex = ii->var;
        node->increment = ii->incr;
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::TABLESWITCH_INSN: {
        auto* ts = ca::core::cast<TableSwitchInsnNode>(insn);
        auto node = std::make_unique<MIRSwitch>();
        node->defaultLabel = ts->dflt->bytecodeOffset;
        int key = ts->min;
        for (auto* l : ts->labels) {
            node->cases.push_back({key++, l->bytecodeOffset});
        }
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::LOOKUPSWITCH_INSN: {
        auto* ls = ca::core::cast<LookupSwitchInsnNode>(insn);
        auto node = std::make_unique<MIRSwitch>();
        node->defaultLabel = ls->dflt->bytecodeOffset;
        for (size_t i = 0; i < ls->keys.size(); i++) {
            node->cases.push_back({ls->keys[i], ls->labels[i]->bytecodeOffset});
        }
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::MULTIANEWARRAY_INSN: {
        // Preprocessor extracts
        cur->nodes.push_back(std::make_unique<MIRNop>());
        break;
    }

    case AbstractInsnNode::FRAME: {
        cur->nodes.push_back(std::make_unique<MIRFrame>());
        break;
    }

    case AbstractInsnNode::LINE:
        // Debug info — skip
        break;

    default:
        break;
    }
}

void CodeConverter::translateInstructions(MethodNode& mn) {
    impl_->currentBlock = nullptr;

    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn) {
        // Check if this insn is a block start
        if (insn->getType() == AbstractInsnNode::LABEL) {
            auto* lb = ca::core::cast<LabelNode>(insn);
            int lid = lb->label->bytecodeOffset;
            auto it = impl_->blocks.find(lid);
            if (it != impl_->blocks.end())
                impl_->currentBlock = it->second.get();
        }

        if (!impl_->currentBlock)
            impl_->currentBlock = getOrCreateBlock(0);

        processInsn(insn);
    }
}

std::vector<Block*> CodeConverter::convert(MethodNode& mn) {
    analyzeControlFlow(mn);
    buildBlocks();
    translateInstructions(mn);
    return impl_->blockOrder;
}

// -- MIR dump --

static const char* mirKindName(MIRKind k) {
    switch (k) {
    case MIRKind::NOP:           return "NOP";
    case MIRKind::LABEL:         return "LABEL";
    case MIRKind::CONSTANT:      return "CONST";
    case MIRKind::LOCAL_LOAD:    return "LOAD";
    case MIRKind::LOCAL_STORE:   return "STORE";
    case MIRKind::IINC:          return "IINC";
    case MIRKind::STACK_DUP:     return "DUP";
    case MIRKind::STACK_DUP_X1:  return "DUP_X1";
    case MIRKind::STACK_DUP_X2:  return "DUP_X2";
    case MIRKind::STACK_DUP2:    return "DUP2";
    case MIRKind::STACK_SWAP:    return "SWAP";
    case MIRKind::BINARY_OP:     return "BINARY";
    case MIRKind::UNARY_OP:      return "UNARY";
    case MIRKind::CAST:          return "CAST";
    case MIRKind::BRANCH:        return "BRANCH";
    case MIRKind::BRANCH_ZERO:   return "BRANCH_ZERO";
    case MIRKind::UNCOND_BRANCH: return "GOTO";
    case MIRKind::SWITCH:        return "SWITCH";
    case MIRKind::RETURN:        return "RETURN";
    case MIRKind::RETURN_VALUE:  return "RETURN_VALUE";
    case MIRKind::NEW_OBJECT:    return "NEW";
    case MIRKind::NEW_ARRAY:     return "NEW_ARRAY";
    case MIRKind::ARRAY_LENGTH:  return "ARRAYLEN";
    case MIRKind::ARRAY_LOAD:    return "ALOAD";
    case MIRKind::ARRAY_STORE:   return "ASTORE";
    case MIRKind::FIELD_GET:     return "GETFIELD";
    case MIRKind::FIELD_PUT:     return "PUTFIELD";
    case MIRKind::STATIC_GET:    return "GETSTATIC";
    case MIRKind::STATIC_PUT:    return "PUTSTATIC";
    case MIRKind::INVOKE:        return "INVOKE";
    case MIRKind::CHECK_CAST:    return "CHECKCAST";
    case MIRKind::INSTANCE_OF:   return "INSTANCEOF";
    case MIRKind::MONITOR_ENTER: return "MONITOR_ENTER";
    case MIRKind::MONITOR_EXIT:  return "MONITOR_EXIT";
    case MIRKind::THROW:         return "ATHROW";
    case MIRKind::FRAME:         return "FRAME";
    default:                     return "???";
    }
}

std::string CodeConverter::dumpMIR(const std::vector<Block*>& blocks) {
    std::ostringstream oss;
    for (auto* blk : blocks) {
        oss << "B" << blk->id << ":\n";
        for (auto& n : blk->nodes) {
            oss << "  " << mirKindName(n->kind);
            switch (n->kind) {
            case MIRKind::LABEL:
                oss << " " << static_cast<MIRLabel*>(n.get())->id;
                break;
            case MIRKind::LOCAL_LOAD:
                oss << " v" << static_cast<MIRLocalLoad*>(n.get())->varIndex;
                break;
            case MIRKind::LOCAL_STORE:
                oss << " v" << static_cast<MIRLocalStore*>(n.get())->varIndex;
                break;
            case MIRKind::IINC:
                oss << " v" << static_cast<MIRIinc*>(n.get())->varIndex;
                break;
            case MIRKind::BRANCH:
                oss << " -> B" << static_cast<MIRBranch*>(n.get())->targetLabel;
                break;
            case MIRKind::BRANCH_ZERO:
                oss << " -> B" << static_cast<MIRBranchZero*>(n.get())->targetLabel;
                break;
            case MIRKind::UNCOND_BRANCH:
                oss << " -> B" << static_cast<MIRUncondBranch*>(n.get())->targetLabel;
                break;
            case MIRKind::NEW_OBJECT:
                oss << " " << static_cast<MIRNewObject*>(n.get())->typeDesc;
                break;
            case MIRKind::FIELD_GET:
            case MIRKind::FIELD_PUT:
                oss << " " << static_cast<MIRFieldGet*>(n.get())->name;
                break;
            case MIRKind::INVOKE:
                oss << " " << static_cast<MIRInvoke*>(n.get())->name;
                break;
            default:
                break;
            }
            oss << "\n";
        }
    }
    return oss.str();
}

} // namespace mj2x

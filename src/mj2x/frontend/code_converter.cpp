#include "code_converter.hpp"
#include "opcode_utils.hpp"
#include "type_utils.hpp"

#include <cassert>
#include <sstream>
#include <string>
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

// label 标识：LabelNode* 转 int（读取期稳定）。
// libjbytecode 树形重建后把所有 label->bytecodeOffset 重置为 -1（insn_list.cpp:390），
// 故不能用 bytecodeOffset 作 label 标识；改用指针身份（见 dev_plan R1a.3）。
static int labelKey(jbytecode::core::Label* lbl) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

struct CodeConverter::Impl {
    std::unordered_map<int, std::unique_ptr<Block>> blocks;
    std::vector<Block*> blockOrder;
    Block* currentBlock = nullptr;
    std::unordered_set<int> labelTargets;
    std::vector<ExceptionHandler> handlers;

    // 块起始指令索引集合（index 在 instruction list 里的位置）。
    // 一个指令 index 是块起始，当且仅当：(a) 它是 LABEL 且其 label 是跳转目标/异常 handler；
    // 或 (b) 它紧跟在某个终结指令（jump/return/athrow/switch）之后（fallthrough 块入口）。
    // 修复"不在终结指令后切 fallthrough 块"的缺陷（见 dev_plan R1a.2）。
    std::unordered_set<int> blockStartIndices;
    int nextSyntheticBlockId = -1000;   // fallthrough 块的合成 id 发放（负数，避开正数 label id）
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
    impl_->blockStartIndices.clear();

    // 收集跳转/switch/异常 handler 目标（块入口）。label 标识用 LabelNode* 转 int。
    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn) {
        switch (insn->getType()) {
        case AbstractInsnNode::JUMP_INSN: {
            auto* jump = typed::cast<JumpInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(jump->label));
            break;
        }
        case AbstractInsnNode::TABLESWITCH_INSN: {
            auto* ts = typed::cast<TableSwitchInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(ts->dflt));
            for (auto* l : ts->labels)
                impl_->labelTargets.insert(labelKey(l));
            break;
        }
        case AbstractInsnNode::LOOKUPSWITCH_INSN: {
            auto* ls = typed::cast<LookupSwitchInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(ls->dflt));
            for (auto* l : ls->labels)
                impl_->labelTargets.insert(labelKey(l));
            break;
        }
        default:
            break;
        }
    }

    for (auto* tcb : mn.tryCatchBlocks) {
        impl_->labelTargets.insert(labelKey(tcb->handler));
        ExceptionHandler h;
        h.startLabel = labelKey(tcb->start);
        h.endLabel = labelKey(tcb->end);
        h.handlerLabel = labelKey(tcb->handler);
        h.catchType = tcb->type.to_std_string();
        impl_->handlers.push_back(h);
    }

    // Entry point：首条指令的 label key（在 translateInstructions 用 afterTerminator 处理，无需预插）

    // 计算 blockStartIndices：遍历指令，按 index 跟踪。
    // (a) 方法入口（首条可执行指令）是块入口；
    // (b) 任何 LABEL 且其 bytecodeOffset 在 labelTargets 里 → 该 LABEL 是块入口（含循环回边目标）；
    // (c) 任何终结指令（jump/return/athrow/switch）之后，下一条可执行指令（非 LABEL/FRAME/LINE）
    //     是 fallthrough 块入口。
    int idx = 0;
    bool afterTerminator = true;   // 入口前视为"终结后"，使首条指令成为块入口
    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn, ++idx) {
        int type = insn->getType();
        bool isPseudo = (type == AbstractInsnNode::LABEL ||
                         type == AbstractInsnNode::FRAME ||
                         type == AbstractInsnNode::LINE);

        if (isPseudo) {
            if (type == AbstractInsnNode::LABEL) {
                auto* lb = typed::cast<LabelNode>(insn);
                if (impl_->labelTargets.count(labelKey(lb->label))) {
                    impl_->blockStartIndices.insert(idx);
                }
            }
            continue;
        }

        if (afterTerminator) {
            impl_->blockStartIndices.insert(idx);
            afterTerminator = false;
        }

        int opc = insn->opcode;
        if (type == AbstractInsnNode::JUMP_INSN ||
            type == AbstractInsnNode::TABLESWITCH_INSN ||
            type == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            afterTerminator = true;
        } else if (type == AbstractInsnNode::INSN &&
                   (opc == Opcodes::IRETURN || opc == Opcodes::LRETURN ||
                    opc == Opcodes::FRETURN || opc == Opcodes::DRETURN ||
                    opc == Opcodes::ARETURN || opc == Opcodes::RETURN ||
                    opc == Opcodes::ATHROW)) {
            afterTerminator = true;
        }
    }
}

void CodeConverter::buildBlocks() {
    impl_->blocks.clear();
    impl_->blockOrder.clear();
    impl_->nextSyntheticBlockId = -1000;
    // 块在 translateInstructions 按 blockStartIndices 边走边建（见下）。
}

void CodeConverter::processInsn(AbstractInsnNode* insn) {
    auto& cur = impl_->currentBlock;
    if (!cur) {
        cur = impl_->blockOrder.empty() ? getOrCreateBlock(0) : impl_->blockOrder[0];
    }

    switch (insn->getType()) {
    case AbstractInsnNode::LABEL: {
        auto* lb = typed::cast<LabelNode>(insn);
        int lid = labelKey(lb->label);
        auto it = impl_->blocks.find(lid);
        if (it != impl_->blocks.end())
            impl_->currentBlock = it->second.get();
        cur->nodes.push_back(std::make_unique<MIRLabel>(lb->label));
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
        auto* ii = typed::cast<IntInsnNode>(insn);
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
        auto* vi = typed::cast<VarInsnNode>(insn);
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
        auto* ti = typed::cast<TypeInsnNode>(insn);
        switch (ti->opcode) {
        case Opcodes::NEW: {
            auto node = std::make_unique<MIRNewObject>();
            node->typeDesc = ti->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::ANEWARRAY: {
            auto node = std::make_unique<MIRNewArray>();
            node->typeDesc = ti->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::CHECKCAST: {
            auto node = std::make_unique<MIRCheckCast>();
            node->typeDesc = ti->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::INSTANCEOF: {
            auto node = std::make_unique<MIRInstanceOf>();
            node->typeDesc = ti->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        default:
            break;
        }
        break;
    }

    case AbstractInsnNode::FIELD_INSN: {
        auto* fi = typed::cast<FieldInsnNode>(insn);
        switch (fi->opcode) {
        case Opcodes::GETSTATIC: {
            auto node = std::make_unique<MIRStaticGet>();
            node->owner = fi->owner.to_std_string(); node->name = fi->name.to_std_string(); node->desc = fi->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTSTATIC: {
            auto node = std::make_unique<MIRStaticPut>();
            node->owner = fi->owner.to_std_string(); node->name = fi->name.to_std_string(); node->desc = fi->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::GETFIELD: {
            auto node = std::make_unique<MIRFieldGet>();
            node->owner = fi->owner.to_std_string(); node->name = fi->name.to_std_string(); node->desc = fi->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTFIELD: {
            auto node = std::make_unique<MIRFieldPut>();
            node->owner = fi->owner.to_std_string(); node->name = fi->name.to_std_string(); node->desc = fi->desc.to_std_string();
            cur->nodes.push_back(std::move(node));
            break;
        }
        default:
            break;
        }
        break;
    }

    case AbstractInsnNode::METHOD_INSN: {
        auto* mi = typed::cast<MethodInsnNode>(insn);
        auto node = std::make_unique<MIRInvoke>();
        node->owner = mi->owner.to_std_string();
        node->name = mi->name.to_std_string();
        node->desc = mi->desc.to_std_string();
        node->isInterface = mi->isInterface;
        node->invokeOpcode = mi->opcode;
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::INVOKE_DYNAMIC_INSN: {
        // R1b: preprocessor 标记但未展开（R2/R5+ 完整展开）。落 MIRInvokeDynamic，
        // builder 遇到则 fail-closed（不静默 MIRNop）。
        auto* id = typed::cast<InvokeDynamicInsnNode>(insn);
        auto node = std::make_unique<MIRInvokeDynamic>();
        node->name = id->name.to_std_string();
        node->desc = id->desc.to_std_string();
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::JUMP_INSN: {
        auto* ji = typed::cast<JumpInsnNode>(insn);
        int opc = ji->opcode;
        jbytecode::core::Label* target = ji->label;   // 用 Label* 身份，不用 bytecodeOffset

        if (opc == Opcodes::GOTO) {
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
        auto* ii = typed::cast<IincInsnNode>(insn);
        auto node = std::make_unique<MIRIinc>();
        node->varIndex = ii->var;
        node->increment = ii->incr;
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::TABLESWITCH_INSN: {
        auto* ts = typed::cast<TableSwitchInsnNode>(insn);
        auto node = std::make_unique<MIRSwitch>();
        node->defaultLabel = ts->dflt;
        int key = ts->min;
        for (auto* l : ts->labels) {
            node->cases.push_back({key++, l});
        }
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::LOOKUPSWITCH_INSN: {
        auto* ls = typed::cast<LookupSwitchInsnNode>(insn);
        auto node = std::make_unique<MIRSwitch>();
        node->defaultLabel = ls->dflt;
        for (size_t i = 0; i < ls->keys.size(); i++) {
            node->cases.push_back({ls->keys[i], ls->labels[i]});
        }
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::MULTIANEWARRAY_INSN: {
        // R1b: 不展开（R2+ 加 runtime helper）。落 MIRMultiNewArray，builder 遇到 fail-closed。
        auto* ma = typed::cast<MultiANewArrayInsnNode>(insn);
        auto node = std::make_unique<MIRMultiNewArray>();
        node->typeDesc = ma->desc.to_std_string();
        node->dims = ma->dims;
        cur->nodes.push_back(std::move(node));
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

    int idx = 0;
    bool firstBlock = true;   // 序列首个块固定 id=0（保持现有契约：entry block id==0）
    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn, ++idx) {
        // 块入口处切换/创建 currentBlock。块 id：
        //   - 序列首个块 → 0（entry 契约）
        //   - LABEL 块 → label bytecodeOffset
        //   - fallthrough 块 → 合成负 id
        if (impl_->blockStartIndices.count(idx)) {
            int blockId;
            if (firstBlock) {
                blockId = 0;
                firstBlock = false;
            } else if (insn->getType() == AbstractInsnNode::LABEL) {
                auto* lb = typed::cast<LabelNode>(insn);
                blockId = labelKey(lb->label);
            } else {
                blockId = impl_->nextSyntheticBlockId--;
            }
            impl_->currentBlock = getOrCreateBlock(blockId);
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
    // 兜底：空方法（无指令）也要有 entry block（保持 id==0 契约）。
    if (impl_->blockOrder.empty()) {
        getOrCreateBlock(0);
    }
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
                oss << " L" << labelKey(static_cast<MIRLabel*>(n.get())->labelNode);
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
                oss << " -> L" << labelKey(static_cast<MIRBranch*>(n.get())->targetLabel);
                break;
            case MIRKind::BRANCH_ZERO:
                oss << " -> L" << labelKey(static_cast<MIRBranchZero*>(n.get())->targetLabel);
                break;
            case MIRKind::UNCOND_BRANCH:
                oss << " -> L" << labelKey(static_cast<MIRUncondBranch*>(n.get())->targetLabel);
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


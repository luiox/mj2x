#include "code_converter.hpp"
#include "opcode_utils.hpp"
#include "type_utils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include "jbytecode/jbytecode.hpp"
#include "libca/core/datatype.hpp"
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
static int labelKey(jbytecode::core::Label* lbl)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

static PrimitiveKind castSourceType(int opcode)
{
    switch (opcode) {
    case Opcodes::I2L:
    case Opcodes::I2F:
    case Opcodes::I2D:
    case Opcodes::I2B:
    case Opcodes::I2C:
    case Opcodes::I2S: return PrimitiveKind::INT;
    case Opcodes::L2I:
    case Opcodes::L2F:
    case Opcodes::L2D: return PrimitiveKind::LONG;
    case Opcodes::F2I:
    case Opcodes::F2L:
    case Opcodes::F2D: return PrimitiveKind::FLOAT;
    case Opcodes::D2I:
    case Opcodes::D2L:
    case Opcodes::D2F: return PrimitiveKind::DOUBLE;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind castTargetType(int opcode)
{
    switch (opcode) {
    case Opcodes::I2L: return PrimitiveKind::LONG;
    case Opcodes::I2F: return PrimitiveKind::FLOAT;
    case Opcodes::I2D: return PrimitiveKind::DOUBLE;
    case Opcodes::L2I: return PrimitiveKind::INT;
    case Opcodes::L2F: return PrimitiveKind::FLOAT;
    case Opcodes::L2D: return PrimitiveKind::DOUBLE;
    case Opcodes::F2I: return PrimitiveKind::INT;
    case Opcodes::F2L: return PrimitiveKind::LONG;
    case Opcodes::F2D: return PrimitiveKind::DOUBLE;
    case Opcodes::D2I: return PrimitiveKind::INT;
    case Opcodes::D2L: return PrimitiveKind::LONG;
    case Opcodes::D2F: return PrimitiveKind::FLOAT;
    case Opcodes::I2B: return PrimitiveKind::BYTE;
    case Opcodes::I2C: return PrimitiveKind::CHAR;
    case Opcodes::I2S: return PrimitiveKind::SHORT;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind arrayComponentType(int opcode)
{
    switch (opcode) {
    case Opcodes::IALOAD:
    case Opcodes::IASTORE: return PrimitiveKind::INT;
    case Opcodes::LALOAD:
    case Opcodes::LASTORE: return PrimitiveKind::LONG;
    case Opcodes::FALOAD:
    case Opcodes::FASTORE: return PrimitiveKind::FLOAT;
    case Opcodes::DALOAD:
    case Opcodes::DASTORE: return PrimitiveKind::DOUBLE;
    case Opcodes::AALOAD:
    case Opcodes::AASTORE: return PrimitiveKind::OBJECT;
    case Opcodes::BALOAD:
    case Opcodes::BASTORE: return PrimitiveKind::BYTE;
    case Opcodes::CALOAD:
    case Opcodes::CASTORE: return PrimitiveKind::CHAR;
    case Opcodes::SALOAD:
    case Opcodes::SASTORE: return PrimitiveKind::SHORT;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind newArrayComponentType(int atype)
{
    switch (atype) {
    case Opcodes::T_BOOLEAN: return PrimitiveKind::BOOLEAN;
    case Opcodes::T_CHAR: return PrimitiveKind::CHAR;
    case Opcodes::T_FLOAT: return PrimitiveKind::FLOAT;
    case Opcodes::T_DOUBLE: return PrimitiveKind::DOUBLE;
    case Opcodes::T_BYTE: return PrimitiveKind::BYTE;
    case Opcodes::T_SHORT: return PrimitiveKind::SHORT;
    case Opcodes::T_INT: return PrimitiveKind::INT;
    case Opcodes::T_LONG: return PrimitiveKind::LONG;
    default: return PrimitiveKind::OBJECT;
    }
}

static PrimitiveKind cmpOperandType(int opcode)
{
    switch (opcode) {
    case Opcodes::LCMP: return PrimitiveKind::LONG;
    case Opcodes::FCMPL:
    case Opcodes::FCMPG: return PrimitiveKind::FLOAT;
    case Opcodes::DCMPL:
    case Opcodes::DCMPG: return PrimitiveKind::DOUBLE;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind frameValueType(const ca::core::Any& value)
{
    if (auto* tag = value.template as<int>()) {
        switch (*tag) {
        case Opcodes::INTEGER: return PrimitiveKind::INT;
        case Opcodes::FLOAT: return PrimitiveKind::FLOAT;
        case Opcodes::LONG: return PrimitiveKind::LONG;
        case Opcodes::DOUBLE: return PrimitiveKind::DOUBLE;
        case Opcodes::TOP: return PrimitiveKind::VOID;
        case Opcodes::ITEM_NULL:
        case Opcodes::UNINITIALIZED_THIS:
        default: return PrimitiveKind::OBJECT;
        }
    }
    // Object and uninitialized verification types are represented by class
    // names or labels in libjbytecode's tree model; both enter SSA as refs.
    return PrimitiveKind::OBJECT;
}

static bool frameHasEntryStack(int frameType)
{
    return frameType == Opcodes::F_NEW || frameType == Opcodes::F_FULL ||
           frameType == Opcodes::F_SAME1;
}

static bool isCategory2Kind(PrimitiveKind kind)
{
    return kind == PrimitiveKind::LONG || kind == PrimitiveKind::DOUBLE;
}

static PrimitiveKind methodReturnPrimitive(const ca::str::Utf8StringRef& desc)
{
    return jvmDescToPrimitive(Type::getReturnType(desc).getDescriptor());
}

static std::vector<PrimitiveKind> methodArgumentPrimitives(const ca::str::Utf8StringRef& desc)
{
    std::vector<PrimitiveKind> result;
    auto                       args = Type::getArgumentTypes(desc);
    result.reserve(args.size());
    for (const auto& arg : args) result.push_back(jvmSortToPrimitive(arg.getSort()));
    return result;
}

struct CodeConverter::Impl
{
    ca::str::Utf8StringArena*                        arena = nullptr;   ///< MIR 字符串字段入池用，借用
    std::unordered_map<int, std::unique_ptr<Block>> blocks;
    std::vector<Block*>                             blockOrder;
    Block*                                          currentBlock = nullptr;
    std::unordered_set<int>                         labelTargets;
    std::vector<ExceptionHandler>                   handlers;
    std::vector<PrimitiveKind>                      operandStack;
    std::unordered_map<int, PrimitiveKind>          localTypes;

    // 块起始指令索引集合（index 在 instruction list 里的位置）。
    // 一个指令 index 是块起始，当且仅当：(a) 它是 LABEL 且其 label 是跳转目标/异常 handler；
    // 或 (b) 它紧跟在某个终结指令（jump/return/athrow/switch）之后（fallthrough 块入口）。
    // 修复"不在终结指令后切 fallthrough 块"的缺陷（见 dev_plan R1a.2）。
    std::unordered_set<int> blockStartIndices;
    int nextSyntheticBlockId = -1000;   // fallthrough 块的合成 id 发放（负数，避开正数 label id）

    void initMethodState(MethodNode& mn)
    {
        operandStack.clear();
        localTypes.clear();
        int slot = 0;
        if (!(mn.access & Opcodes::ACC_STATIC)) localTypes[slot++] = PrimitiveKind::OBJECT;
        auto desc = Type::getMethodType(mn.desc);
        for (const auto& argType : desc.getArgumentTypes()) {
            auto kind        = jvmSortToPrimitive(argType.getSort());
            localTypes[slot] = kind;
            slot += primitiveStackSize(kind);
        }
    }

    void push(PrimitiveKind kind)
    {
        if (kind != PrimitiveKind::VOID) operandStack.push_back(kind);
    }

    PrimitiveKind pop()
    {
        if (operandStack.empty()) return PrimitiveKind::INT;
        auto kind = operandStack.back();
        operandStack.pop_back();
        return kind;
    }

    PrimitiveKind peek(size_t depth = 0) const
    {
        if (depth >= operandStack.size()) return PrimitiveKind::INT;
        return operandStack[operandStack.size() - 1 - depth];
    }

    std::vector<PrimitiveKind> topSnapshot(size_t limit) const
    {
        std::vector<PrimitiveKind> result;
        const size_t               count = std::min(limit, operandStack.size());
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) result.push_back(peek(i));
        return result;
    }

    void resetStack(const std::vector<PrimitiveKind>& stackTypes) { operandStack = stackTypes; }
};

CodeConverter::CodeConverter(ca::str::Utf8StringArena& arena)
    : impl_(std::make_unique<Impl>())
{
    impl_->arena = &arena;
}

CodeConverter::~CodeConverter() = default;

Block* CodeConverter::getOrCreateBlock(int labelId)
{
    auto it = impl_->blocks.find(labelId);
    if (it != impl_->blocks.end()) return it->second.get();
    auto  block            = std::make_unique<Block>(labelId);
    auto* ptr              = block.get();
    impl_->blocks[labelId] = std::move(block);
    impl_->blockOrder.push_back(ptr);
    return ptr;
}

void CodeConverter::analyzeControlFlow(MethodNode& mn)
{
    impl_->labelTargets.clear();
    impl_->handlers.clear();
    impl_->blockStartIndices.clear();

    // 收集跳转/switch/异常 handler 目标（块入口）。label 标识用 LabelNode* 转 int。
    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn) {
        switch (insn->getType()) {
        case AbstractInsnNode::JUMP_INSN:
        {
            auto* jump = typed::cast<JumpInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(jump->label));
            break;
        }
        case AbstractInsnNode::TABLESWITCH_INSN:
        {
            auto* ts = typed::cast<TableSwitchInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(ts->dflt));
            for (auto* l : ts->labels) impl_->labelTargets.insert(labelKey(l));
            break;
        }
        case AbstractInsnNode::LOOKUPSWITCH_INSN:
        {
            auto* ls = typed::cast<LookupSwitchInsnNode>(insn);
            impl_->labelTargets.insert(labelKey(ls->dflt));
            for (auto* l : ls->labels) impl_->labelTargets.insert(labelKey(l));
            break;
        }
        default: break;
        }
    }

    for (auto* tcb : mn.tryCatchBlocks) {
        impl_->labelTargets.insert(labelKey(tcb->start));
        impl_->labelTargets.insert(labelKey(tcb->end));
        impl_->labelTargets.insert(labelKey(tcb->handler));
        ExceptionHandler h;
        h.startLabel   = labelKey(tcb->start);
        h.endLabel     = labelKey(tcb->end);
        h.handlerLabel = labelKey(tcb->handler);
        h.catchType    = impl_->arena->intern(tcb->type);
        impl_->handlers.push_back(h);
    }

    // Entry point：首条指令的 label key（在 translateInstructions 用 afterTerminator
    // 处理，无需预插）

    // 计算 blockStartIndices：遍历指令，按 index 跟踪。
    // (a) 方法入口（首条可执行指令）是块入口；
    // (b) 任何 LABEL 且其 bytecodeOffset 在 labelTargets 里 → 该 LABEL 是块入口（含循环回边目标）；
    // (c) 任何终结指令（jump/return/athrow/switch）之后，下一条可执行指令（非 LABEL/FRAME/LINE）
    //     是 fallthrough 块入口。
    int  idx             = 0;
    bool afterTerminator = true;   // 入口前视为"终结后"，使首条指令成为块入口
    for (auto* insn = mn.instructions.getFirst(); insn; insn = insn->nextInsn, ++idx) {
        int  type     = insn->getType();
        bool isPseudo = (type == AbstractInsnNode::LABEL || type == AbstractInsnNode::FRAME ||
                         type == AbstractInsnNode::LINE);

        if (isPseudo) {
            if (type == AbstractInsnNode::LABEL) {
                auto* lb = typed::cast<LabelNode>(insn);
                if (impl_->labelTargets.count(labelKey(lb->label))) {
                    impl_->blockStartIndices.insert(idx);
                    // The label itself starts the block; keep the following
                    // executable instruction in the same block so handler
                    // entry stack state is not split away from ASTORE.
                    afterTerminator = false;
                }
            }
            continue;
        }

        if (afterTerminator) {
            impl_->blockStartIndices.insert(idx);
            afterTerminator = false;
        }

        int opc = insn->opcode;
        if (type == AbstractInsnNode::JUMP_INSN || type == AbstractInsnNode::TABLESWITCH_INSN ||
            type == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            afterTerminator = true;
        }
        else if (type == AbstractInsnNode::INSN &&
                 (opc == Opcodes::IRETURN || opc == Opcodes::LRETURN || opc == Opcodes::FRETURN ||
                  opc == Opcodes::DRETURN || opc == Opcodes::ARETURN || opc == Opcodes::RETURN ||
                  opc == Opcodes::ATHROW)) {
            afterTerminator = true;
        }
    }
}

void CodeConverter::buildBlocks()
{
    impl_->blocks.clear();
    impl_->blockOrder.clear();
    impl_->nextSyntheticBlockId = -1000;
    // 块在 translateInstructions 按 blockStartIndices 边走边建（见下）。
}

void CodeConverter::processInsn(AbstractInsnNode* insn)
{
    auto& cur = impl_->currentBlock;
    if (!cur) {
        cur = impl_->blockOrder.empty() ? getOrCreateBlock(0) : impl_->blockOrder[0];
    }
    auto addStackOp = [&](MIRKind kind) {
        auto node            = std::make_unique<MIRStackOp>(kind);
        node->stackBeforeTop = impl_->topSnapshot(4);
        cur->nodes.push_back(std::move(node));

        switch (kind) {
        case MIRKind::STACK_POP: impl_->pop(); break;
        case MIRKind::STACK_POP2:
        {
            auto v1 = impl_->pop();
            if (!isCategory2Kind(v1)) impl_->pop();
            break;
        }
        case MIRKind::STACK_DUP: impl_->push(impl_->peek()); break;
        case MIRKind::STACK_DUP_X1:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            impl_->push(v1);
            impl_->push(v2);
            impl_->push(v1);
            break;
        }
        case MIRKind::STACK_DUP_X2:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            if (isCategory2Kind(v2)) {
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            else {
                auto v3 = impl_->pop();
                impl_->push(v1);
                impl_->push(v3);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                impl_->push(v1);
                impl_->push(v1);
            }
            else {
                auto v2 = impl_->pop();
                impl_->push(v2);
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2_X1:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                auto v2 = impl_->pop();
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            else {
                auto v2 = impl_->pop();
                auto v3 = impl_->pop();
                impl_->push(v2);
                impl_->push(v1);
                impl_->push(v3);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2_X2:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                auto v2 = impl_->pop();
                if (isCategory2Kind(v2)) {
                    impl_->push(v1);
                    impl_->push(v2);
                    impl_->push(v1);
                }
                else {
                    auto v3 = impl_->pop();
                    impl_->push(v1);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
            }
            else {
                auto v2 = impl_->pop();
                auto v3 = impl_->pop();
                if (isCategory2Kind(v3)) {
                    impl_->push(v2);
                    impl_->push(v1);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
                else {
                    auto v4 = impl_->pop();
                    impl_->push(v2);
                    impl_->push(v1);
                    impl_->push(v4);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
            }
            break;
        }
        case MIRKind::STACK_SWAP:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            impl_->push(v1);
            impl_->push(v2);
            break;
        }
        default: break;
        }
    };

    switch (insn->getType()) {
    case AbstractInsnNode::LABEL:
    {
        auto* lb  = typed::cast<LabelNode>(insn);
        int   lid = labelKey(lb->label);
        auto  it  = impl_->blocks.find(lid);
        if (it != impl_->blocks.end()) impl_->currentBlock = it->second.get();
        cur->nodes.push_back(std::make_unique<MIRLabel>(lb->label));
        break;
    }

    case AbstractInsnNode::INSN:
    {
        int opc = insn->opcode;
        if (opc == Opcodes::NOP) {
            cur->nodes.push_back(std::make_unique<MIRNop>());
        }
        else if (opc == Opcodes::ACONST_NULL) {
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::OBJECT;
            node->value = nullptr;
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (opc >= Opcodes::ICONST_M1 && opc <= Opcodes::ICONST_5) {
            // ICONST_M1..ICONST_5：int 常量 -1..5
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::INT;
            node->value = static_cast<int64_t>(opc - Opcodes::ICONST_0);
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (opc == Opcodes::LCONST_0 || opc == Opcodes::LCONST_1) {
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::LONG;
            node->value = static_cast<int64_t>(opc - Opcodes::LCONST_0);
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (opc == Opcodes::FCONST_0 || opc == Opcodes::FCONST_1 || opc == Opcodes::FCONST_2) {
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::FLOAT;
            node->value = static_cast<double>(opc - Opcodes::FCONST_0);
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (opc == Opcodes::DCONST_0 || opc == Opcodes::DCONST_1) {
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::DOUBLE;
            node->value = static_cast<double>(opc - Opcodes::DCONST_0);
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (opc >= Opcodes::IALOAD && opc <= Opcodes::SALOAD) {
            auto node           = std::make_unique<MIRArrayLoad>();
            node->componentType = arrayComponentType(opc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
            impl_->push(arrayComponentType(opc));
        }
        else if (opc >= Opcodes::IASTORE && opc <= Opcodes::SASTORE) {
            auto node           = std::make_unique<MIRArrayStore>();
            node->componentType = arrayComponentType(opc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
            impl_->pop();
        }
        else if (opc == Opcodes::POP) {
            addStackOp(MIRKind::STACK_POP);
        }
        else if (opc == Opcodes::POP2) {
            addStackOp(MIRKind::STACK_POP2);
        }
        else if (opc == Opcodes::DUP) {
            addStackOp(MIRKind::STACK_DUP);
        }
        else if (opc == Opcodes::DUP_X1) {
            addStackOp(MIRKind::STACK_DUP_X1);
        }
        else if (opc == Opcodes::DUP_X2) {
            addStackOp(MIRKind::STACK_DUP_X2);
        }
        else if (opc == Opcodes::DUP2) {
            addStackOp(MIRKind::STACK_DUP2);
        }
        else if (opc == Opcodes::DUP2_X1) {
            addStackOp(MIRKind::STACK_DUP2_X1);
        }
        else if (opc == Opcodes::DUP2_X2) {
            addStackOp(MIRKind::STACK_DUP2_X2);
        }
        else if (opc == Opcodes::SWAP) {
            addStackOp(MIRKind::STACK_SWAP);
        }
        else if ((opc >= Opcodes::IADD && opc <= Opcodes::DREM) ||
                 (opc >= Opcodes::ISHL && opc <= Opcodes::LXOR)) {
            auto node  = std::make_unique<MIRBinaryOp>();
            node->op   = opcodeToBinaryOp(opc);
            node->type = opcodeToResultType(opc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
            impl_->push(opcodeToResultType(opc));
        }
        else if (opc >= Opcodes::INEG && opc <= Opcodes::DNEG) {
            auto node  = std::make_unique<MIRUnaryOp>();
            node->op   = opcodeToUnaryOp(opc);
            node->type = opcodeToResultType(opc);
            cur->nodes.push_back(std::move(node));
        }
        else if ((opc >= Opcodes::I2L && opc <= Opcodes::D2F) || opc == Opcodes::I2B ||
                 opc == Opcodes::I2C || opc == Opcodes::I2S) {
            auto node                = std::make_unique<MIRCast>();
            node->from               = castSourceType(opc);
            node->to                 = castTargetType(opc);
            PrimitiveKind targetType = node->to;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->push(targetType);
        }
        else if (opc == Opcodes::LCMP || opc == Opcodes::FCMPL || opc == Opcodes::FCMPG ||
                 opc == Opcodes::DCMPL || opc == Opcodes::DCMPG) {
            auto node        = std::make_unique<MIRCmp>();
            node->type       = cmpOperandType(opc);
            node->nanGreater = (opc == Opcodes::FCMPG || opc == Opcodes::DCMPG);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
            impl_->push(PrimitiveKind::INT);
        }
        else if (opc >= Opcodes::IRETURN && opc <= Opcodes::ARETURN) {
            auto node = std::make_unique<MIRReturnValue>();
            // 按 *RETURN opcode 推导返回值类型（与方法描述符一致）。
            auto retKind = [](int op) -> PrimitiveKind {
                switch (op) {
                case Opcodes::IRETURN: return PrimitiveKind::INT;
                case Opcodes::LRETURN: return PrimitiveKind::LONG;
                case Opcodes::FRETURN: return PrimitiveKind::FLOAT;
                case Opcodes::DRETURN: return PrimitiveKind::DOUBLE;
                case Opcodes::ARETURN: return PrimitiveKind::OBJECT;
                default: return PrimitiveKind::INT;
                }
            };
            node->type = retKind(opc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
        }
        else if (opc == Opcodes::RETURN) {
            cur->nodes.push_back(std::make_unique<MIRReturn>());
        }
        else if (opc == Opcodes::ARRAYLENGTH) {
            cur->nodes.push_back(std::make_unique<MIRArrayLength>());
            impl_->pop();
            impl_->push(PrimitiveKind::INT);
        }
        else if (opc == Opcodes::ATHROW) {
            cur->nodes.push_back(std::make_unique<MIRThrow>());
            impl_->pop();
        }
        else if (opc == Opcodes::MONITORENTER) {
            auto node = std::make_unique<MIRMonitor>();   // enter=true default
            cur->nodes.push_back(std::move(node));
            impl_->pop();
        }
        else if (opc == Opcodes::MONITOREXIT) {
            auto node   = std::make_unique<MIRMonitor>();
            node->enter = false;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
        }
        break;
    }

    case AbstractInsnNode::INT_INSN:
    {
        auto* ii = typed::cast<IntInsnNode>(insn);
        if (ii->opcode == Opcodes::BIPUSH || ii->opcode == Opcodes::SIPUSH) {
            auto node   = std::make_unique<MIRConstant>();
            node->type  = PrimitiveKind::INT;
            node->value = static_cast<int64_t>(ii->operand);
            impl_->push(node->type);
            cur->nodes.push_back(std::move(node));
        }
        else if (ii->opcode == Opcodes::NEWARRAY) {
            auto node           = std::make_unique<MIRNewArray>();
            node->componentType = newArrayComponentType(ii->operand);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->push(PrimitiveKind::OBJECT);
        }
        break;
    }

    case AbstractInsnNode::VAR_INSN:
    {
        auto* vi  = typed::cast<VarInsnNode>(insn);
        int   opc = vi->opcode;
        // 局部变量类型直接由 opcode 推导（比 MethodStack 更精确：MethodStack 只跟踪参数 slot）。
        // ILOAD/LLOAD/FLOAD/DLOAD/ALOAD 与对应 STORE 是连续 opcode（21..25 / 54..58）。
        auto varKind = [](int op) -> PrimitiveKind {
            switch (op) {
            case Opcodes::ILOAD:
            case Opcodes::ISTORE: return PrimitiveKind::INT;
            case Opcodes::LLOAD:
            case Opcodes::LSTORE: return PrimitiveKind::LONG;
            case Opcodes::FLOAD:
            case Opcodes::FSTORE: return PrimitiveKind::FLOAT;
            case Opcodes::DLOAD:
            case Opcodes::DSTORE: return PrimitiveKind::DOUBLE;
            case Opcodes::ALOAD:
            case Opcodes::ASTORE: return PrimitiveKind::OBJECT;
            default: return PrimitiveKind::INT;
            }
        };
        if (opc >= Opcodes::ILOAD && opc <= Opcodes::ALOAD) {
            auto node      = std::make_unique<MIRLocalLoad>();
            node->varIndex = vi->var;
            node->type     = varKind(opc);
            cur->nodes.push_back(std::move(node));
            impl_->push(varKind(opc));
        }
        else if (opc >= Opcodes::ISTORE && opc <= Opcodes::ASTORE) {
            auto node      = std::make_unique<MIRLocalStore>();
            node->varIndex = vi->var;
            node->type     = varKind(opc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->localTypes[vi->var] = varKind(opc);
        }
        else if (opc == Opcodes::RET) {
            // JSR/RET subroutines are outside the MVP bytecode surface.  Keep
            // an explicit unsupported node so backends fail closed instead of
            // silently dropping the instruction as metadata.
            auto node  = std::make_unique<MIRInvokeDynamic>();
            node->name = impl_->arena->intern("ret");
            node->desc = impl_->arena->intern("()V");
            cur->nodes.push_back(std::move(node));
        }
        break;
    }

    case AbstractInsnNode::TYPE_INSN:
    {
        auto* ti = typed::cast<TypeInsnNode>(insn);
        switch (ti->opcode) {
        case Opcodes::NEW:
        {
            auto node      = std::make_unique<MIRNewObject>();
            node->typeDesc = impl_->arena->intern(ti->desc);
            cur->nodes.push_back(std::move(node));
            impl_->push(PrimitiveKind::OBJECT);
            break;
        }
        case Opcodes::ANEWARRAY:
        {
            auto node           = std::make_unique<MIRNewArray>();
            node->typeDesc      = impl_->arena->intern(ti->desc);
            node->componentType = PrimitiveKind::OBJECT;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->push(PrimitiveKind::OBJECT);
            break;
        }
        case Opcodes::CHECKCAST:
        {
            auto node      = std::make_unique<MIRCheckCast>();
            node->typeDesc = impl_->arena->intern(ti->desc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->push(PrimitiveKind::OBJECT);
            break;
        }
        case Opcodes::INSTANCEOF:
        {
            auto node      = std::make_unique<MIRInstanceOf>();
            node->typeDesc = impl_->arena->intern(ti->desc);
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->push(PrimitiveKind::INT);
            break;
        }
        default: break;
        }
        break;
    }

    case AbstractInsnNode::FIELD_INSN:
    {
        auto* fi = typed::cast<FieldInsnNode>(insn);
        switch (fi->opcode) {
        case Opcodes::GETSTATIC:
        {
            auto node   = std::make_unique<MIRStaticGet>();
            node->owner = impl_->arena->intern(fi->owner);
            node->name  = impl_->arena->intern(fi->name);
            node->desc  = impl_->arena->intern(fi->desc);
            impl_->push(jvmDescToPrimitive(node->desc));
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTSTATIC:
        {
            auto node   = std::make_unique<MIRStaticPut>();
            node->owner = impl_->arena->intern(fi->owner);
            node->name  = impl_->arena->intern(fi->name);
            node->desc  = impl_->arena->intern(fi->desc);
            impl_->pop();
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::GETFIELD:
        {
            auto node   = std::make_unique<MIRFieldGet>();
            node->owner = impl_->arena->intern(fi->owner);
            node->name  = impl_->arena->intern(fi->name);
            node->desc  = impl_->arena->intern(fi->desc);
            impl_->pop();
            impl_->push(jvmDescToPrimitive(node->desc));
            cur->nodes.push_back(std::move(node));
            break;
        }
        case Opcodes::PUTFIELD:
        {
            auto node   = std::make_unique<MIRFieldPut>();
            node->owner = impl_->arena->intern(fi->owner);
            node->name  = impl_->arena->intern(fi->name);
            node->desc  = impl_->arena->intern(fi->desc);
            impl_->pop();
            impl_->pop();
            cur->nodes.push_back(std::move(node));
            break;
        }
        default: break;
        }
        break;
    }

    case AbstractInsnNode::METHOD_INSN:
    {
        auto* mi           = typed::cast<MethodInsnNode>(insn);
        auto  node         = std::make_unique<MIRInvoke>();
        node->owner        = impl_->arena->intern(mi->owner);
        node->name         = impl_->arena->intern(mi->name);
        node->desc         = impl_->arena->intern(mi->desc);
        node->isInterface  = mi->isInterface;
        node->invokeOpcode = mi->opcode;
        auto args          = methodArgumentPrimitives(node->desc);
        for (size_t i = 0; i < args.size(); ++i) impl_->pop();
        if (mi->opcode != Opcodes::INVOKESTATIC) impl_->pop();
        impl_->push(methodReturnPrimitive(node->desc));
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::INVOKE_DYNAMIC_INSN:
    {
        auto* id      = typed::cast<InvokeDynamicInsnNode>(insn);
        auto  node    = std::make_unique<MIRInvokeDynamic>();
        node->name    = impl_->arena->intern(id->name);
        node->desc    = impl_->arena->intern(id->desc);
        node->bsm     = id->bsm;
        node->bsmArgs = id->bsmArgs;
        auto args     = methodArgumentPrimitives(node->desc);
        for (size_t i = 0; i < args.size(); ++i) impl_->pop();
        impl_->push(methodReturnPrimitive(node->desc));
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::JUMP_INSN:
    {
        auto*                   ji     = typed::cast<JumpInsnNode>(insn);
        int                     opc    = ji->opcode;
        jbytecode::core::Label* target = ji->label;   // 用 Label* 身份，不用 bytecodeOffset

        if (opc == Opcodes::GOTO) {
            auto node         = std::make_unique<MIRUncondBranch>();
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        }
        else if (opc == Opcodes::IF_ACMPEQ || opc == Opcodes::IF_ACMPNE) {
            auto node         = std::make_unique<MIRBranch>();
            node->op          = branchOpcodeToCmpOp(opc);
            node->type        = PrimitiveKind::OBJECT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
        }
        else if (opc >= Opcodes::IF_ICMPEQ && opc <= Opcodes::IF_ICMPLE) {
            auto node         = std::make_unique<MIRBranch>();
            node->op          = branchOpcodeToCmpOp(opc);
            node->type        = PrimitiveKind::INT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
        }
        else if (opc == Opcodes::IFNULL || opc == Opcodes::IFNONNULL ||
                 (opc >= Opcodes::IFEQ && opc <= Opcodes::IFLE)) {
            auto node         = std::make_unique<MIRBranchZero>();
            node->op          = branchZeroOpcodeToCmpOp(opc);
            node->type        = (opc == Opcodes::IFNULL || opc == Opcodes::IFNONNULL)
                                    ? PrimitiveKind::OBJECT
                                    : PrimitiveKind::INT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
        }
        else {
            // JSR/JSR_W subroutines are unsupported for the MVP.  Route them
            // through the same explicit fail-closed boundary as invokedynamic.
            auto node  = std::make_unique<MIRInvokeDynamic>();
            node->name = impl_->arena->intern("jsr");
            node->desc = impl_->arena->intern("()V");
            cur->nodes.push_back(std::move(node));
        }
        break;
    }

    case AbstractInsnNode::LDC_INSN:
    {
        // LDC 常量类型（来自 ClassReader::readConst）：
        //   int → i32, float → float, long → i64, double → double,
        //   String → Utf8StringRef, Class/MethodType → Type, MethodHandle → Handle。
        // Wave 4：String LDC 保留字面量，由 StackC 生成 NewStringUTF。
        // Class/MethodType 以 Type 传递；Handle 仍暂落 null OBJECT。
        auto* ld   = typed::cast<LdcInsnNode>(insn);
        auto  node = std::make_unique<MIRConstant>();
        if (auto* p = ld->cst.template as<ca::i32>()) {
            node->type  = PrimitiveKind::INT;
            node->value = static_cast<int64_t>(*p);
        }
        else if (auto* p = ld->cst.template as<ca::i64>()) {
            node->type  = PrimitiveKind::LONG;
            node->value = static_cast<int64_t>(*p);
        }
        else if (auto* p = ld->cst.template as<float>()) {
            node->type  = PrimitiveKind::FLOAT;
            node->value = static_cast<double>(*p);
        }
        else if (auto* p = ld->cst.template as<double>()) {
            node->type  = PrimitiveKind::DOUBLE;
            node->value = *p;
        }
        else if (auto* p = ld->cst.template as<std::string>()) {
            node->type  = PrimitiveKind::OBJECT;
            node->value = impl_->arena->intern(ca::str::Utf8StringRef::from_string_view(*p));
        }
        else if (auto* p = ld->cst.template as<ca::str::Utf8StringRef>()) {
            node->type  = PrimitiveKind::OBJECT;
            node->value = impl_->arena->intern(*p);
        }
        else if (auto* p = ld->cst.template as<Type>()) {
            node->type  = PrimitiveKind::OBJECT;
            node->value = *p;
        }
        else {
            // Handle：暂落 null 对象常量。
            node->type  = PrimitiveKind::OBJECT;
            node->value = nullptr;
        }
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::IINC_INSN:
    {
        auto* ii        = typed::cast<IincInsnNode>(insn);
        auto  node      = std::make_unique<MIRIinc>();
        node->varIndex  = ii->var;
        node->increment = ii->incr;
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::TABLESWITCH_INSN:
    {
        auto* ts           = typed::cast<TableSwitchInsnNode>(insn);
        auto  node         = std::make_unique<MIRSwitch>();
        node->defaultLabel = ts->dflt;
        int key            = ts->min;
        for (auto* l : ts->labels) {
            node->cases.push_back({key++, l});
        }
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        break;
    }

    case AbstractInsnNode::LOOKUPSWITCH_INSN:
    {
        auto* ls           = typed::cast<LookupSwitchInsnNode>(insn);
        auto  node         = std::make_unique<MIRSwitch>();
        node->defaultLabel = ls->dflt;
        for (size_t i = 0; i < ls->keys.size(); i++) {
            node->cases.push_back({ls->keys[i], ls->labels[i]});
        }
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        break;
    }

    case AbstractInsnNode::MULTIANEWARRAY_INSN:
    {
        // Keep the bytecode compact: downstream SSA/RegC lowers this to the
        // multi-array runtime helper instead of expanding nested loops here.
        auto* ma       = typed::cast<MultiANewArrayInsnNode>(insn);
        auto  node     = std::make_unique<MIRMultiNewArray>();
        node->typeDesc = impl_->arena->intern(ma->desc);
        node->dims     = ma->dims;
        cur->nodes.push_back(std::move(node));
        for (int i = 0; i < ma->dims; ++i) impl_->pop();
        impl_->push(PrimitiveKind::OBJECT);
        break;
    }

    case AbstractInsnNode::FRAME:
    {
        auto* frame     = typed::cast<FrameNode>(insn);
        auto  node      = std::make_unique<MIRFrame>();
        node->frameType = frame->frameType;
        if (frameHasEntryStack(frame->frameType)) {
            node->stackTypes.reserve(frame->stack.size());
            for (const auto& value : frame->stack) {
                PrimitiveKind kind = frameValueType(value);
                if (kind != PrimitiveKind::VOID) {
                    node->stackTypes.push_back(kind);
                    if (frame->frameType == Opcodes::F_SAME1) break;
                }
            }
        }
        if (frameHasEntryStack(frame->frameType)) impl_->resetStack(node->stackTypes);
        cur->nodes.push_back(std::move(node));
        break;
    }

    case AbstractInsnNode::LINE:
        // Debug info — skip
        break;

    default: break;
    }
}

void CodeConverter::translateInstructions(MethodNode& mn)
{
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
                blockId    = 0;
                firstBlock = false;
            }
            else if (insn->getType() == AbstractInsnNode::LABEL) {
                auto* lb = typed::cast<LabelNode>(insn);
                blockId  = labelKey(lb->label);
            }
            else {
                blockId = impl_->nextSyntheticBlockId--;
            }
            impl_->currentBlock = getOrCreateBlock(blockId);
        }

        if (!impl_->currentBlock) impl_->currentBlock = getOrCreateBlock(0);

        processInsn(insn);
    }
}

std::vector<Block*> CodeConverter::convert(MethodNode& mn)
{
    impl_->initMethodState(mn);
    analyzeControlFlow(mn);
    buildBlocks();
    translateInstructions(mn);
    // 兜底：空方法（无指令）也要有 entry block（保持 id==0 契约）。
    if (impl_->blockOrder.empty()) {
        getOrCreateBlock(0);
    }
    if (!impl_->blockOrder.empty()) {
        impl_->blockOrder.front()->exceptionTable = impl_->handlers;
    }
    return impl_->blockOrder;
}

// -- MIR dump --

static const char* mirKindName(MIRKind k)
{
    switch (k) {
    case MIRKind::NOP: return "NOP";
    case MIRKind::LABEL: return "LABEL";
    case MIRKind::CONSTANT: return "CONST";
    case MIRKind::LOCAL_LOAD: return "LOAD";
    case MIRKind::LOCAL_STORE: return "STORE";
    case MIRKind::IINC: return "IINC";
    case MIRKind::STACK_DUP: return "DUP";
    case MIRKind::STACK_DUP_X1: return "DUP_X1";
    case MIRKind::STACK_DUP_X2: return "DUP_X2";
    case MIRKind::STACK_DUP2: return "DUP2";
    case MIRKind::STACK_DUP2_X1: return "DUP2_X1";
    case MIRKind::STACK_DUP2_X2: return "DUP2_X2";
    case MIRKind::STACK_SWAP: return "SWAP";
    case MIRKind::STACK_POP: return "POP";
    case MIRKind::STACK_POP2: return "POP2";
    case MIRKind::BINARY_OP: return "BINARY";
    case MIRKind::UNARY_OP: return "UNARY";
    case MIRKind::CMP: return "CMP";
    case MIRKind::CAST: return "CAST";
    case MIRKind::BRANCH: return "BRANCH";
    case MIRKind::BRANCH_ZERO: return "BRANCH_ZERO";
    case MIRKind::UNCOND_BRANCH: return "GOTO";
    case MIRKind::SWITCH: return "SWITCH";
    case MIRKind::RETURN: return "RETURN";
    case MIRKind::RETURN_VALUE: return "RETURN_VALUE";
    case MIRKind::NEW_OBJECT: return "NEW";
    case MIRKind::NEW_ARRAY: return "NEW_ARRAY";
    case MIRKind::ARRAY_LENGTH: return "ARRAYLEN";
    case MIRKind::ARRAY_LOAD: return "ALOAD";
    case MIRKind::ARRAY_STORE: return "ASTORE";
    case MIRKind::FIELD_GET: return "GETFIELD";
    case MIRKind::FIELD_PUT: return "PUTFIELD";
    case MIRKind::STATIC_GET: return "GETSTATIC";
    case MIRKind::STATIC_PUT: return "PUTSTATIC";
    case MIRKind::INVOKE: return "INVOKE";
    case MIRKind::CHECK_CAST: return "CHECKCAST";
    case MIRKind::INSTANCE_OF: return "INSTANCEOF";
    case MIRKind::MONITOR_ENTER: return "MONITOR_ENTER";
    case MIRKind::MONITOR_EXIT: return "MONITOR_EXIT";
    case MIRKind::THROW: return "ATHROW";
    case MIRKind::FRAME: return "FRAME";
    default: return "???";
    }
}

static const char* primitiveKindName(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "boolean";
    case PrimitiveKind::BYTE: return "byte";
    case PrimitiveKind::CHAR: return "char";
    case PrimitiveKind::SHORT: return "short";
    case PrimitiveKind::INT: return "int";
    case PrimitiveKind::LONG: return "long";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    case PrimitiveKind::OBJECT: return "object";
    }
    return "?";
}

std::string CodeConverter::dumpMIR(const std::vector<Block*>& blocks)
{
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
            case MIRKind::IINC: oss << " v" << static_cast<MIRIinc*>(n.get())->varIndex; break;
            case MIRKind::FRAME:
            {
                auto* frame = static_cast<MIRFrame*>(n.get());
                oss << " type=" << frame->frameType << " stack=[";
                for (size_t i = 0; i < frame->stackTypes.size(); ++i) {
                    if (i) oss << ",";
                    oss << primitiveKindName(frame->stackTypes[i]);
                }
                oss << "]";
                break;
            }
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
            case MIRKind::FIELD_PUT: oss << " " << static_cast<MIRFieldGet*>(n.get())->name; break;
            case MIRKind::INVOKE: oss << " " << static_cast<MIRInvoke*>(n.get())->name; break;
            default: break;
            }
            oss << "\n";
        }
    }
    return oss.str();
}

}   // namespace mj2x

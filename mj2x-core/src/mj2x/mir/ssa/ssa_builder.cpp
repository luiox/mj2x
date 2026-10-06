#include "ssa_builder.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"

#include <cassert>
#include <algorithm>
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include "libca/str/utf8_string.hpp"

namespace mj2x {

using namespace jbytecode::core;

namespace {

// ---- FrameState：builder-local 构造期栈模拟。不进 IR。吸收 DUP/SWAP。 ----
struct FrameState
{
    std::vector<SSAValue*> locals;   // local slot -> 当前绑定的 SSA value
    std::vector<SSAValue*> stack;    // 操作数栈（纯数据结构）

    explicit FrameState(int maxLocals)
        : locals(maxLocals, nullptr)
    {}

    void      push(SSAValue* v) { stack.push_back(v); }
    SSAValue* pop()
    {
        // 栈下溢 = builder bug 或 L1 问题；返回 opaque 防崩，调用方应 fail-closed。
        if (stack.empty()) return nullptr;
        SSAValue* v = stack.back();
        stack.pop_back();
        return v;
    }
};

// 把 SSAType 从 PrimitiveKind 映射（R1 保守，细化推 R2）。
SSAType mirKindToSSA(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return SSAType::BOOL;
    case PrimitiveKind::BYTE: return SSAType::I8;
    case PrimitiveKind::CHAR: return SSAType::CHAR;
    case PrimitiveKind::SHORT: return SSAType::I16;
    case PrimitiveKind::INT: return SSAType::I32;
    case PrimitiveKind::LONG: return SSAType::I64;
    case PrimitiveKind::FLOAT: return SSAType::F32;
    case PrimitiveKind::DOUBLE: return SSAType::F64;
    case PrimitiveKind::VOID: return SSAType::VOID;
    case PrimitiveKind::OBJECT: return SSAType::REF;
    default: return SSAType::UNKNOWN;
    }
}

SSAType jvmTypeToSSA(const Type& type)
{
    return mirKindToSSA(jvmSortToPrimitive(type.getSort()));
}

SSAType descToSSA(const ca::str::Utf8StringRef& desc)
{
    return mirKindToSSA(jvmDescToPrimitive(desc));
}

std::string primitiveArrayDesc(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "[Z";
    case PrimitiveKind::BYTE: return "[B";
    case PrimitiveKind::CHAR: return "[C";
    case PrimitiveKind::SHORT: return "[S";
    case PrimitiveKind::INT: return "[I";
    case PrimitiveKind::LONG: return "[J";
    case PrimitiveKind::FLOAT: return "[F";
    case PrimitiveKind::DOUBLE: return "[D";
    default: return "[Ljava/lang/Object;";
    }
}

SSAValue* constantValue(SSAFunction& fn, const MIRConstant* c)
{
    if (std::holds_alternative<int64_t>(c->value)) {
        return fn.internInt(std::get<int64_t>(c->value), mirKindToSSA(c->type));
    }
    if (std::holds_alternative<double>(c->value)) {
        auto type = c->type == PrimitiveKind::DOUBLE ? SSAType::F64 : SSAType::F32;
        return fn.internFloat(std::get<double>(c->value), type);
    }
    if (std::holds_alternative<std::nullptr_t>(c->value)) {
        return fn.internNull();
    }
    if (std::holds_alternative<ca::str::Utf8StringRef>(c->value)) {
        // Carrying the literal content through the opaque tag lets downstream
        // String intrinsics (parseInt/literal/toString) recover it without a
        // separate IR value kind.  The "ldc_string:" prefix marks this opaque
        // as a String LDC; anything after it is the raw UTF-8 content.  Emitter
        // and dump apply their own escaping when rendering.
        return fn.newOpaque(SSAType::REF, "ldc_string:" + std::get<ca::str::Utf8StringRef>(c->value).to_std_string());
    }
    if (std::holds_alternative<jbytecode::core::Type>(c->value)) {
        // LDC Class 常量（Foo.class）：Type 的 internal name 即 JNI FindClass 需要的路径
        // （如 "java/lang/String"）。用 "ldc_class:" 前缀透传给 emitter 发射 FindClass。
        const auto& t = std::get<jbytecode::core::Type>(c->value);
        // #921 staticArena 退役：内部名仅瞬时转串做 SSA 名，函数级局部 arena 零驻留。
        ca::str::Utf8StringArena scratch;
        return fn.newOpaque(SSAType::REF, "ldc_class:" + t.getInternalName(scratch).to_std_string());
    }
    return fn.newOpaque(mirKindToSSA(c->type), "ldc_unknown");
}

std::string arrayDescFor(const MIRNewArray* node)
{
    if (!node->typeDesc.is_empty()) {
        const auto desc = node->typeDesc.to_std_string();
        if (desc[0] == '[') return "[" + desc;
        return "[L" + desc + ";";
    }
    return primitiveArrayDesc(node->componentType);
}

bool isStaticInvoke(int opcode)
{
    return opcode == Opcodes::INVOKESTATIC;
}

void markResolvingReadEffect(SSAInstruction* instr)
{
    instr->effect.mayThrow         = true;
    instr->effect.readsMemory      = true;
    instr->effect.resolvesExternal = true;
}

void markResolvingWriteEffect(SSAInstruction* instr)
{
    instr->effect.mayThrow         = true;
    instr->effect.writesMemory     = true;
    instr->effect.resolvesExternal = true;
}

void markRuntimeCheckEffect(SSAInstruction* instr)
{
    instr->effect.mayThrow         = true;
    instr->effect.resolvesExternal = true;
}

SSAType localDefType(const std::vector<Block*>& blocks, int blockLabel, int nodeIndex)
{
    for (auto* block : blocks) {
        if (block->id != blockLabel) continue;
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(block->nodes.size())) break;
        const auto& node = block->nodes[static_cast<size_t>(nodeIndex)];
        if (node->kind == MIRKind::LOCAL_STORE) {
            return mirKindToSSA(ca::core::tag_cast::cast<MIRLocalStore>(node.get())->type);
        }
        if (node->kind == MIRKind::IINC) {
            return SSAType::I32;
        }
        break;
    }
    return SSAType::UNKNOWN;
}

SSAType blockArgType(const std::vector<Block*>& blocks, const DefSet& defs)
{
    SSAType inferred = SSAType::UNKNOWN;
    for (const auto& def : defs) {
        SSAType next = localDefType(blocks, def.blockLabel, def.nodeIndex);
        if (next == SSAType::UNKNOWN) continue;
        if (inferred == SSAType::UNKNOWN) {
            inferred = next;
        }
        else if (inferred != next) {
            return SSAType::UNKNOWN;
        }
    }
    return inferred == SSAType::UNKNOWN ? SSAType::I32 : inferred;
}

bool isBlockMetadata(MIRKind kind)
{
    return kind == MIRKind::LABEL || kind == MIRKind::FRAME || kind == MIRKind::LINE ||
           kind == MIRKind::NOP;
}

void appendFrameStackTypes(std::vector<SSAType>& out, const MIRFrame* frame)
{
    if (!frame) return;
    out.reserve(frame->stackTypes.size());
    for (PrimitiveKind kind : frame->stackTypes) {
        out.push_back(mirKindToSSA(kind));
    }
}

bool blockHasStackArgs(const SSABlock* block)
{
    if (!block) return false;
    for (const auto& arg : block->args) {
        if (isSSAStackValueSlot(arg.slot)) return true;
    }
    return false;
}

void ensureSuccessorStackArgs(SSABlock* target, const FrameState& fs, SSAFunction& fn)
{
    if (!target || fs.stack.empty() || blockHasStackArgs(target)) return;

    // Some verifier-split fallthrough blocks, such as the ireturn after a
    // throwing GETFIELD inside a try range, have no StackMapTable entry of
    // their own.  The predecessor FrameState is still authoritative because
    // the target is translated later in L1 order.
    for (size_t i = 0; i < fs.stack.size(); ++i) {
        SSAType type = fs.stack[i] ? fs.stack[i]->type : SSAType::UNKNOWN;
        target->args.emplace_back(
            fn.ids.newValueId(), type, ssaStackValueSlot(static_cast<int>(i)));
    }
}

std::vector<SSAType> stackArgTypes(const std::vector<Block*>& blocks, size_t blockIndex)
{
    std::vector<SSAType> result;
    if (blockIndex >= blocks.size()) return result;
    const Block* block = blocks[blockIndex];

    for (const auto& node : block->nodes) {
        if (node->kind == MIRKind::FRAME) {
            appendFrameStackTypes(result, static_cast<const MIRFrame*>(node.get()));
            return result;
        }
        if (!isBlockMetadata(node->kind)) break;
    }

    if (blockIndex == 0) return result;
    const Block* previous = blocks[blockIndex - 1];
    if (!previous) return result;
    for (auto it = previous->nodes.rbegin(); it != previous->nodes.rend(); ++it) {
        MIRKind kind = (*it)->kind;
        if (kind == MIRKind::FRAME) {
            appendFrameStackTypes(result, static_cast<const MIRFrame*>(it->get()));
            return result;
        }
        if (kind == MIRKind::LABEL || kind == MIRKind::LINE || kind == MIRKind::NOP) continue;
        break;
    }
    return result;
}

bool isCategory2(const SSAValue* value)
{
    return value && ssaStackSize(value->type) == 2;
}

bool isCategory1(const SSAValue* value)
{
    return value && ssaStackSize(value->type) == 1;
}

// 算术 op 类型推断（R1：整型统一 I32，长整型 I64，浮点 F32/F64）。
SSAType binaryResultType(SSAType lhs, SSAType rhs)
{
    if (lhs == SSAType::I64 || rhs == SSAType::I64) return SSAType::I64;
    if (lhs == SSAType::F64 || rhs == SSAType::F64) return SSAType::F64;
    if (lhs == SSAType::F32 || rhs == SSAType::F32) return SSAType::F32;
    return SSAType::I32;
}

// MIR BinaryOp → SSA BinaryOp（枚举顺序不同，必须显式映射）。
SSABinaryOp mapBinaryOp(BinaryOp op)
{
    switch (op) {
    case BinaryOp::ADD: return SSABinaryOp::Add;
    case BinaryOp::SUB: return SSABinaryOp::Sub;
    case BinaryOp::MUL: return SSABinaryOp::Mul;
    case BinaryOp::DIV: return SSABinaryOp::Div;
    case BinaryOp::REM: return SSABinaryOp::Rem;
    case BinaryOp::SHL: return SSABinaryOp::Shl;
    case BinaryOp::SHR: return SSABinaryOp::Shr;
    case BinaryOp::USHR: return SSABinaryOp::UShr;
    case BinaryOp::AND: return SSABinaryOp::And;
    case BinaryOp::OR: return SSABinaryOp::Or;
    case BinaryOp::XOR: return SSABinaryOp::Xor;
    }
    return SSABinaryOp::Add;   // 不可达
}

// MIR CmpOp → SSAComparePredicate（EQ,NE,LT,GE,GT,LE → Eq,Ne,Lt,Le,Gt,Ge）。
SSAComparePredicate mapComparePredicate(CmpOp op)
{
    switch (op) {
    case CmpOp::EQ: return SSAComparePredicate::Eq;
    case CmpOp::NE: return SSAComparePredicate::Ne;
    case CmpOp::LT: return SSAComparePredicate::Lt;
    case CmpOp::GE: return SSAComparePredicate::Ge;
    case CmpOp::GT: return SSAComparePredicate::Gt;
    case CmpOp::LE: return SSAComparePredicate::Le;
    }
    return SSAComparePredicate::Eq;   // 不可达
}

// 构造一条出边：跳到 target 块，并按 target.args 的 slot 从 FrameState.locals/stack 取值传出。
// target=null 时返回空 successor（不可达/错误兜底）。
SSASuccessor buildSuccessor(SSABlock* target, const FrameState& fs, SSAFunction& fn)
{
    SSASuccessor s;
    s.block = target;
    if (!target) return s;
    ensureSuccessorStackArgs(target, fs, fn);
    for (auto& arg : target->args) {
        if (arg.slot >= 0 && arg.slot < static_cast<int>(fs.locals.size()) && fs.locals[arg.slot]) {
            s.args.push_back(fs.locals[arg.slot]);
        }
        else if (isSSAStackValueSlot(arg.slot)) {
            int depth = ssaStackValueDepth(arg.slot);
            if (depth >= 0 && depth < static_cast<int>(fs.stack.size()) && fs.stack[depth]) {
                s.args.push_back(fs.stack[depth]);
            }
            else {
                s.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "undef_stack_arg"));
            }
        }
        else {
            s.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "undef_succ_arg"));
        }
    }
    return s;
}

// 找 lb 在 l1blocks 序列里的下一个块对应的 SSABlock（fallthrough 目标）。
SSABlock* fallthroughBlock(const std::vector<Block*>&                l1blocks,
                           const std::unordered_map<int, SSABlock*>& ssaByLabel, Block* lb)
{
    for (size_t i = 0; i < l1blocks.size(); ++i) {
        if (l1blocks[i] == lb && i + 1 < l1blocks.size()) {
            auto it = ssaByLabel.find(l1blocks[i + 1]->id);
            return it != ssaByLabel.end() ? it->second : nullptr;
        }
    }
    return nullptr;
}

// 按 Label* 解析跳转目标。javac 对"回边到方法首条指令"（如 while(true) 循环
// 的 goto 0）只合成 Label 对象、不发 LABEL 指令，CodeConverter 不会为该方法
// 入口块建立该 label 映射，ssaByLabel 查不到。此时目标语义 = 方法真入口块
// （fn.blocks[0]，合成 entry 之前的第一个 L1 块），回退到它并保持 args 绑定。
SSABlock* resolveJumpTarget(const std::unordered_map<int, SSABlock*>& ssaByLabel, int labelKey,
                            SSABlock* realEntry)
{
    auto it = ssaByLabel.find(labelKey);
    return it != ssaByLabel.end() ? it->second : realEntry;
}

// targetLabel is a Label*; CodeConverter stores label identity as the
// pointer-derived key because libjbytecode resets bytecode offsets during tree
// reconstruction.
int mirLabelKey(jbytecode::core::Label* lbl)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

bool blockContainsLabel(const Block* block, int labelId)
{
    if (!block) return false;
    if (block->id == labelId) return true;
    for (const auto& node : block->nodes) {
        if (node->kind != MIRKind::LABEL) continue;
        auto* label = static_cast<const MIRLabel*>(node.get());
        if (mirLabelKey(label->labelNode) == labelId) return true;
    }
    return false;
}

/// @brief 块是否落在异常表保护区 `[startLabel, endLabel)` 内。
///
/// @details
/// 判定真源 = 指令序号区间（ExceptionHandler::startInsnIdx/endInsnIdx 与
/// Block::firstInsnIdx/lastInsnIdx，同一条指令列表的下标）：块的指令范围与
/// 保护区相交即受保护。这与 JVM 指令级 `[start_pc, end_pc)` 语义对齐，且不
/// 依赖"块序 == 字节码序"的 label 区间扫描（旧实现：从含 startLabel 的块到
/// 含 endLabel 的块之间所有块都算保护，块序乱序或 label 与指令同块时会扩大
/// 保护区）。start/end 指令序号未解析（-1）时退化为旧 label 扫描语义
/// （防御，宁可过度保护也不漏保护）。
bool blockInProtectedRange(const std::vector<Block*>& blocks, const ExceptionHandler& handler,
                           const Block* needle)
{
    if (needle == nullptr) return false;
    if (handler.startInsnIdx >= 0 && handler.endInsnIdx >= 0 && needle->firstInsnIdx >= 0 &&
        needle->lastInsnIdx >= 0) {
        // [start, end) 半开区间与块指令范围相交（含块首伪指令 idx，同尺一致）。
        return needle->firstInsnIdx < handler.endInsnIdx &&
               needle->lastInsnIdx >= handler.startInsnIdx;
    }
    // 未解析 fallback：旧语义（块序区间扫描）。
    bool active = false;
    for (auto* block : blocks) {
        if (blockContainsLabel(block, handler.startLabel)) active = true;
        if (blockContainsLabel(block, handler.endLabel)) active = false;
        if (active && block == needle) return true;
    }
    return false;
}

bool isExceptionHandlerBlock(const std::vector<ExceptionHandler>& handlers, const Block* block)
{
    return std::any_of(handlers.begin(), handlers.end(), [&](const ExceptionHandler& handler) {
        return blockContainsLabel(block, handler.handlerLabel);
    });
}

Block* findBlockByLabel(const std::vector<Block*>& blocks, int labelId)
{
    for (auto* block : blocks) {
        if (blockContainsLabel(block, labelId)) return block;
    }
    return nullptr;
}

SSABlockArg* findExceptionArg(SSABlock* block)
{
    if (!block) return nullptr;
    for (auto& arg : block->args) {
        if (arg.slot == kSSAExceptionObjectSlot) return &arg;
    }
    return nullptr;
}

// ============================================================
// per-MIRKind lowering helpers
// ------------------------------------------------------------
// translateBlocks 的内层 switch 拆成按 JVM 语义族分组的 lower 函数。
// 每个 lower 函数返回 bool：false = fail-closed（reason 已填），true = 成功。
// 所有函数共享匿名 namespace 的 free helper（constantValue / mapBinaryOp /
// buildSuccessor / mirLabelKey 等），状态通过 LowerCtx 显式传递。
// ============================================================

// lowering 阶段的可变上下文：translateBlocks 主循环为每个块构造一次。
struct LowerCtx
{
    SSAFunction&                              fn;
    SSABlock*&                                sb;          // 当前块的指令累积目标
    FrameState&                               fs;          // 构造期栈模拟
    const std::unordered_map<int, SSABlock*>& ssaByLabel;  // l1Label -> SSABlock*
    const std::vector<Block*>&                l1blocks;    // fallthrough 查询用
    Block*                                    lb;          // 当前 L1 块
    ca::str::Utf8StringArena&                 arena;       ///< 派生串（如数组 desc）入池用
};

// ---- 分区 1：常量与局部（CONSTANT / LOCAL_LOAD / LOCAL_STORE / IINC） ----

bool lowerConstant(const MIRConstant* c, LowerCtx& d, std::string& /*reason*/)
{
    // LDC String/Class 等对象常量先落 opaque ref；具体对象构造留给 RegC/runtime lowering。
    d.fs.push(constantValue(d.fn, c));
    return true;
}

bool lowerLocalLoad(const MIRLocalLoad* ll, LowerCtx& d, std::string& /*reason*/)
{
    if (ll->varIndex < static_cast<int>(d.fs.locals.size()) && d.fs.locals[ll->varIndex]) {
        d.fs.push(d.fs.locals[ll->varIndex]);
    }
    else {
        // 未定义到达 = opaque 兜底
        d.fs.push(d.fn.newOpaque(mirKindToSSA(ll->type),
                                  "undef_local_" + std::to_string(ll->varIndex)));
    }
    return true;
}

bool lowerLocalStore(const MIRLocalStore* ls, LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at LOCAL_STORE";
        return false;
    }
    if (ls->varIndex < static_cast<int>(d.fs.locals.size())) {
        d.fs.locals[ls->varIndex] = v;
    }
    return true;
}

bool lowerIinc(const MIRIinc* ii, LowerCtx& d, std::string& reason)
{
    SSAValue* lhs = (ii->varIndex < static_cast<int>(d.fs.locals.size()))
                        ? d.fs.locals[ii->varIndex]
                        : nullptr;
    if (!lhs) {
        reason = "iinc on undef local";
        return false;
    }
    auto instr          = std::make_unique<SSABinaryInstruction>();
    instr->op           = SSABinaryOp::Add;
    instr->lhs          = lhs;
    instr->rhs          = d.fn.internInt(ii->increment, SSAType::I32);
    instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::I32);
    instr->hasResult    = true;
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    if (ii->varIndex < static_cast<int>(d.fs.locals.size()))
        d.fs.locals[ii->varIndex] = resultPtr;
    return true;
}

// ---- 分区 3：算术与比较（BINARY_OP / UNARY_OP / CMP / CAST） ----

bool lowerBinaryOp(const MIRBinaryOp* bo, LowerCtx& d, std::string& reason)
{
    SSAValue* rhs = d.fs.pop();
    SSAValue* lhs = d.fs.pop();
    if (!lhs || !rhs) {
        reason = "stack underflow at BINARY_OP";
        return false;
    }
    auto instr       = std::make_unique<SSABinaryInstruction>();
    instr->op        = mapBinaryOp(bo->op);
    instr->lhs       = lhs;
    instr->rhs       = rhs;
    SSAType rt       = binaryResultType(lhs->type, rhs->type);
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), rt);
    instr->hasResult = true;
    if (instr->op == SSABinaryOp::Div || instr->op == SSABinaryOp::Rem) {
        instr->effect.mayThrow = true;
    }
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerUnaryOp(const MIRUnaryOp* uo, LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at UNARY_OP";
        return false;
    }
    auto instr          = std::make_unique<SSAUnaryInstruction>();
    instr->op           = SSAUnaryOp::Neg;
    instr->value        = v;
    instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), v->type);
    instr->hasResult    = true;
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

// ---- 分区 2：栈操作（只动 FrameState，不产 IR；消除 cstack 锚点的核心） ----

bool lowerStackPop(LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "underflow POP";
        return false;
    }
    return true;
}

bool lowerStackPop2(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    if (!v1) {
        reason = "underflow POP2";
        return false;
    }
    if (!isCategory2(v1)) {
        SSAValue* v2 = d.fs.pop();
        if (!v2) {
            reason = "underflow POP2 category-1 pair";
            return false;
        }
        if (!isCategory1(v2)) {
            reason = "invalid POP2 category mix";
            return false;
        }
    }
    return true;
}

bool lowerStackDup(LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "underflow DUP";
        return false;
    }
    if (!isCategory1(v)) {
        reason = "DUP requires category-1 value";
        return false;
    }
    d.fs.push(v);
    d.fs.push(v);
    return true;
}

bool lowerStackSwap(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    SSAValue* v2 = d.fs.pop();
    if (!v1 || !v2) {
        reason = "underflow SWAP";
        return false;
    }
    if (!isCategory1(v1) || !isCategory1(v2)) {
        reason = "SWAP requires two category-1 values";
        return false;
    }
    d.fs.push(v1);
    d.fs.push(v2);
    return true;
}

bool lowerStackDupX1(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    SSAValue* v2 = d.fs.pop();
    if (!v1 || !v2) {
        reason = "underflow DUP_X1";
        return false;
    }
    if (!isCategory1(v1) || !isCategory1(v2)) {
        reason = "DUP_X1 requires two category-1 values";
        return false;
    }
    d.fs.push(v1);
    d.fs.push(v2);
    d.fs.push(v1);
    return true;
}

bool lowerStackDupX2(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    if (!v1) {
        reason = "underflow DUP_X2";
        return false;
    }
    if (!isCategory1(v1)) {
        reason = "DUP_X2 top must be category-1";
        return false;
    }
    SSAValue* v2 = d.fs.pop();
    if (!v2) {
        reason = "underflow DUP_X2";
        return false;
    }
    if (isCategory2(v2)) {
        d.fs.push(v1);
        d.fs.push(v2);
        d.fs.push(v1);
    }
    else if (isCategory1(v2)) {
        SSAValue* v3 = d.fs.pop();
        if (!v3) {
            reason = "underflow DUP_X2 category-1 form";
            return false;
        }
        if (!isCategory1(v3)) {
            reason = "invalid DUP_X2 category mix";
            return false;
        }
        d.fs.push(v1);
        d.fs.push(v3);
        d.fs.push(v2);
        d.fs.push(v1);
    }
    else {
        reason = "invalid DUP_X2 value category";
        return false;
    }
    return true;
}

bool lowerStackDup2(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    if (!v1) {
        reason = "underflow DUP2";
        return false;
    }
    if (isCategory2(v1)) {
        d.fs.push(v1);
        d.fs.push(v1);
    }
    else if (isCategory1(v1)) {
        SSAValue* v2 = d.fs.pop();
        if (!v2) {
            reason = "underflow DUP2 category-1 pair";
            return false;
        }
        if (!isCategory1(v2)) {
            reason = "invalid DUP2 category mix";
            return false;
        }
        d.fs.push(v2);
        d.fs.push(v1);
        d.fs.push(v2);
        d.fs.push(v1);
    }
    else {
        reason = "invalid DUP2 value category";
        return false;
    }
    return true;
}

bool lowerStackDup2X1(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    if (!v1) {
        reason = "underflow DUP2_X1";
        return false;
    }
    if (isCategory2(v1)) {
        SSAValue* v2 = d.fs.pop();
        if (!v2) {
            reason = "underflow DUP2_X1 category-2 form";
            return false;
        }
        if (!isCategory1(v2)) {
            reason = "invalid DUP2_X1 category mix";
            return false;
        }
        d.fs.push(v1);
        d.fs.push(v2);
        d.fs.push(v1);
    }
    else if (isCategory1(v1)) {
        SSAValue* v2 = d.fs.pop();
        SSAValue* v3 = d.fs.pop();
        if (!v2 || !v3) {
            reason = "underflow DUP2_X1 category-1 form";
            return false;
        }
        if (!isCategory1(v2) || !isCategory1(v3)) {
            reason = "DUP2_X1 category-1 form requires three category-1 values";
            return false;
        }
        d.fs.push(v2);
        d.fs.push(v1);
        d.fs.push(v3);
        d.fs.push(v2);
        d.fs.push(v1);
    }
    else {
        reason = "invalid DUP2_X1 value category";
        return false;
    }
    return true;
}

bool lowerStackDup2X2(LowerCtx& d, std::string& reason)
{
    SSAValue* v1 = d.fs.pop();
    if (!v1) {
        reason = "underflow DUP2_X2";
        return false;
    }
    if (isCategory2(v1)) {
        SSAValue* v2 = d.fs.pop();
        if (!v2) {
            reason = "underflow DUP2_X2 category-2 top form";
            return false;
        }
        if (isCategory2(v2)) {
            d.fs.push(v1);
            d.fs.push(v2);
            d.fs.push(v1);
        }
        else if (isCategory1(v2)) {
            SSAValue* v3 = d.fs.pop();
            if (!v3) {
                reason = "underflow DUP2_X2 mixed top form";
                return false;
            }
            if (!isCategory1(v3)) {
                reason = "invalid DUP2_X2 category mix";
                return false;
            }
            d.fs.push(v1);
            d.fs.push(v3);
            d.fs.push(v2);
            d.fs.push(v1);
        }
        else {
            reason = "invalid DUP2_X2 value category";
            return false;
        }
    }
    else if (isCategory1(v1)) {
        SSAValue* v2 = d.fs.pop();
        if (!v2) {
            reason = "underflow DUP2_X2 category-1 top form";
            return false;
        }
        if (!isCategory1(v2)) {
            reason = "invalid DUP2_X2 category-1 pair";
            return false;
        }
        SSAValue* v3 = d.fs.pop();
        if (!v3) {
            reason = "underflow DUP2_X2 insertion form";
            return false;
        }
        if (isCategory2(v3)) {
            d.fs.push(v2);
            d.fs.push(v1);
            d.fs.push(v3);
            d.fs.push(v2);
            d.fs.push(v1);
        }
        else if (isCategory1(v3)) {
            SSAValue* v4 = d.fs.pop();
            if (!v4) {
                reason = "underflow DUP2_X2 four-value form";
                return false;
            }
            if (!isCategory1(v4)) {
                reason = "invalid DUP2_X2 four-value form";
                return false;
            }
            d.fs.push(v2);
            d.fs.push(v1);
            d.fs.push(v4);
            d.fs.push(v3);
            d.fs.push(v2);
            d.fs.push(v1);
        }
        else {
            reason = "invalid DUP2_X2 insertion category";
            return false;
        }
    }
    else {
        reason = "invalid DUP2_X2 value category";
        return false;
    }
    return true;
}

// ---- 分区 3 续：算术与比较 ----

bool lowerCast(const MIRCast* cast, LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at CAST";
        return false;
    }
    auto instr          = std::make_unique<SSAConvertInstruction>();
    instr->convertKind  = SSAConvertKind::Numeric;
    instr->value        = v;
    instr->targetType   = mirKindToSSA(cast->to);
    instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), instr->targetType);
    instr->hasResult    = true;
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerCmp(const MIRCmp* cmp, LowerCtx& d, std::string& reason)
{
    SSAValue* rhs = d.fs.pop();
    SSAValue* lhs = d.fs.pop();
    if (!lhs || !rhs) {
        reason = "stack underflow at CMP";
        return false;
    }
    auto instr  = std::make_unique<SSAIntrinsicInstruction>();
    instr->name = cmp->nanGreater ? "cmp.nan_greater" : "cmp.nan_less";
    instr->args.push_back(lhs);
    instr->args.push_back(rhs);
    instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::I32);
    instr->hasResult    = true;
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

// ---- 分区 4：控制转移（RETURN / RETURN_VALUE / UNCOND_BRANCH / BRANCH / BRANCH_ZERO / SWITCH / THROW） ----

bool lowerReturn(LowerCtx& d, std::string& /*reason*/)
{
    d.sb->terminator = SSAReturnTerminator{nullptr};
    return true;
}

bool lowerReturnValue(const MIRReturnValue* /*rv*/, LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at RETURN_VALUE";
        return false;
    }
    d.sb->terminator = SSAReturnTerminator{v};
    return true;
}

bool lowerUncondBranch(const MIRUncondBranch* ub, LowerCtx& d, std::string& /*reason*/)
{
    int               tkey   = mirLabelKey(ub->targetLabel);
    SSABlock*         target = resolveJumpTarget(d.ssaByLabel, tkey, d.fn.blocks.empty() ? nullptr : d.fn.blocks[0].get());
    SSAJumpTerminator jt;
    jt.target      = buildSuccessor(target, d.fs, d.fn);
    d.sb->terminator = std::move(jt);
    return true;
}
bool lowerBranchZero(const MIRBranchZero* bz, LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at BRANCH_ZERO";
        return false;
    }
    auto cmp       = std::make_unique<SSACompareInstruction>();
    cmp->predicate = mapComparePredicate(bz->op);
    cmp->lhs       = v;
    SSAValue* zero =
        (bz->type == PrimitiveKind::OBJECT)
            ? static_cast<SSAValue*>(d.fn.internNull())
            : static_cast<SSAValue*>(d.fn.internInt(0, mirKindToSSA(bz->type)));
    cmp->rhs       = zero;
    cmp->result    = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::BOOL);
    cmp->hasResult = true;
    SSAValue* cond = &cmp->result;
    d.sb->instructions.push_back(std::move(cmp));
    SSABranchTerminator bt;
    bt.condition  = cond;
    int bzKey     = mirLabelKey(bz->targetLabel);
    bt.trueTarget = buildSuccessor(
        resolveJumpTarget(d.ssaByLabel, bzKey, d.fn.blocks.empty() ? nullptr : d.fn.blocks[0].get()), d.fs, d.fn);
    bt.falseTarget = buildSuccessor(fallthroughBlock(d.l1blocks, d.ssaByLabel, d.lb), d.fs, d.fn);
    d.sb->terminator = std::move(bt);
    return true;
}

bool lowerBranch(const MIRBranch* br, LowerCtx& d, std::string& reason)
{
    SSAValue* rhs = d.fs.pop();
    SSAValue* lhs = d.fs.pop();
    if (!lhs || !rhs) {
        reason = "stack underflow at BRANCH";
        return false;
    }
    auto cmp       = std::make_unique<SSACompareInstruction>();
    cmp->predicate = mapComparePredicate(br->op);
    cmp->lhs       = lhs;
    cmp->rhs       = rhs;
    cmp->result    = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::BOOL);
    cmp->hasResult = true;
    SSAValue* cond = &cmp->result;
    d.sb->instructions.push_back(std::move(cmp));
    SSABranchTerminator bt;
    bt.condition  = cond;
    int brKey     = mirLabelKey(br->targetLabel);
    bt.trueTarget = buildSuccessor(
        resolveJumpTarget(d.ssaByLabel, brKey, d.fn.blocks.empty() ? nullptr : d.fn.blocks[0].get()), d.fs, d.fn);
    bt.falseTarget = buildSuccessor(fallthroughBlock(d.l1blocks, d.ssaByLabel, d.lb), d.fs, d.fn);
    d.sb->terminator = std::move(bt);
    return true;
}

bool lowerSwitch(const MIRSwitch* sw, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    if (!value) {
        reason = "stack underflow at SWITCH";
        return false;
    }
    SSASwitchTerminator st;
    st.value = value;
    for (const auto& c : sw->cases) {
        int           key = mirLabelKey(c.targetLabel);
        SSASwitchCase sc;
        sc.key    = c.key;
        sc.target = buildSuccessor(
            resolveJumpTarget(d.ssaByLabel, key, d.fn.blocks.empty() ? nullptr : d.fn.blocks[0].get()), d.fs, d.fn);
        st.cases.push_back(std::move(sc));
    }
    int defaultKey   = mirLabelKey(sw->defaultLabel);
    st.defaultTarget = buildSuccessor(
        resolveJumpTarget(d.ssaByLabel, defaultKey, d.fn.blocks.empty() ? nullptr : d.fn.blocks[0].get()), d.fs, d.fn);
    d.sb->terminator = std::move(st);
    return true;
}

bool lowerThrow(LowerCtx& d, std::string& reason)
{
    SSAValue* v = d.fs.pop();
    if (!v) {
        reason = "stack underflow at THROW";
        return false;
    }
    d.sb->terminator = SSAThrowTerminator{v};
    return true;
}

// ---- 分区 5：字段与静态（FIELD_GET / FIELD_PUT / STATIC_GET / STATIC_PUT） ----

bool lowerFieldGet(const MIRFieldGet* fg, LowerCtx& d, std::string& reason)
{
    SSAValue* receiver = d.fs.pop();
    if (!receiver) {
        reason = "stack underflow at FIELD_GET";
        return false;
    }
    auto instr       = std::make_unique<SSALoadFieldInstruction>();
    instr->field     = SSAFieldRef{fg->owner, fg->name, fg->desc, false};
    instr->receiver  = receiver;
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), descToSSA(fg->desc));
    instr->hasResult = true;
    markResolvingReadEffect(instr.get());
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerFieldPut(const MIRFieldPut* fp, LowerCtx& d, std::string& reason)
{
    SSAValue* value    = d.fs.pop();
    SSAValue* receiver = d.fs.pop();
    if (!receiver || !value) {
        reason = "stack underflow at FIELD_PUT";
        return false;
    }
    auto instr      = std::make_unique<SSAStoreFieldInstruction>();
    instr->field    = SSAFieldRef{fp->owner, fp->name, fp->desc, false};
    instr->receiver = receiver;
    instr->value    = value;
    markResolvingWriteEffect(instr.get());
    d.sb->instructions.push_back(std::move(instr));
    return true;
}

bool lowerStaticGet(const MIRStaticGet* sg, LowerCtx& d, std::string& /*reason*/)
{
    auto instr      = std::make_unique<SSALoadFieldInstruction>();
    instr->field     = SSAFieldRef{sg->owner, sg->name, sg->desc, true};
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), descToSSA(sg->desc));
    instr->hasResult = true;
    markResolvingReadEffect(instr.get());
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerStaticPut(const MIRStaticPut* sp, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    if (!value) {
        reason = "stack underflow at STATIC_PUT";
        return false;
    }
    auto instr   = std::make_unique<SSAStoreFieldInstruction>();
    instr->field = SSAFieldRef{sp->owner, sp->name, sp->desc, true};
    instr->value = value;
    markResolvingWriteEffect(instr.get());
    d.sb->instructions.push_back(std::move(instr));
    return true;
}

// ---- 分区 6：调用与分配（INVOKE / NEW_OBJECT / NEW_ARRAY / MULTI_NEW_ARRAY） ----

bool lowerInvoke(const MIRInvoke* inv, LowerCtx& d, std::string& reason)
{
    auto argTypes = Type::getArgumentTypes(inv->desc);
    auto retType  = jvmTypeToSSA(Type::getReturnType(inv->desc));

    auto instr    = std::make_unique<SSACallInstruction>();
    instr->method = SSAMethodRef{inv->owner, inv->name, inv->desc, inv->invokeOpcode};
    instr->args.resize(argTypes.size() + (isStaticInvoke(inv->invokeOpcode) ? 0 : 1),
                       nullptr);

    // JVM 调用参数以最后一个实参在栈顶；SSA call args 保持 receiver,arg0,arg1...
    // 的源码顺序，便于 RegC 后续直接生成调用 ABI。
    for (int i = static_cast<int>(argTypes.size()) - 1; i >= 0; --i) {
        SSAValue* arg = d.fs.pop();
        if (!arg) {
            reason = "stack underflow at INVOKE args";
            return false;
        }
        instr->args[static_cast<size_t>(
            i + (isStaticInvoke(inv->invokeOpcode) ? 0 : 1))] = arg;
    }
    if (!isStaticInvoke(inv->invokeOpcode)) {
        SSAValue* receiver = d.fs.pop();
        if (!receiver) {
            reason = "stack underflow at INVOKE receiver";
            return false;
        }
        instr->args[0] = receiver;
    }

    instr->effect                  = SSAEffect::externalCall();
    instr->effect.resolvesExternal = true;
    if (retType != SSAType::VOID) {
        instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), retType);
        instr->hasResult    = true;
        SSAValue* resultPtr = &instr->result;
        d.sb->instructions.push_back(std::move(instr));
        d.fs.push(resultPtr);
    }
    else {
        d.sb->instructions.push_back(std::move(instr));
    }
    return true;
}

/// invokedynamic（自定义 bootstrap，阶段3）：无法在编译期静态展开，登记站点元数据
/// 并降级为 Intrinsic("indy:<index>")，由 RegC emit 生成运行时 bootstrap 调用。
bool lowerInvokeDynamic(const MIRInvokeDynamic* indy, LowerCtx& d, std::string& reason)
{
    auto argTypes = Type::getArgumentTypes(indy->desc);
    auto retType  = jvmTypeToSSA(Type::getReturnType(indy->desc));

    SSAFunction::InvokeDynamicSite site;
    site.name    = indy->name.to_std_string();
    site.desc    = indy->desc.to_std_string();
    site.bsm     = indy->bsm;
    site.bsmArgs = indy->bsmArgs;
    // indy native dispatch（设计文档 §6 P1）：站点级判定填充直派载荷（与
    // classifyIndySite 同一判定主体，判定单源）。只填载荷不改写行为；
    // RuntimeHelper / 载荷未 engage 的站点发射端走既有阶段3 helper 路径。
    const IndyDispatchPayload payload =
        classifyIndySiteForm(d.arena, site.name, site.desc, indy->bsm, indy->bsmArgs);
    site.form          = payload.form;
    site.directField   = payload.directField;
    site.recordMethods = payload.recordMethods;
    const size_t siteIndex = d.fn.indySites.size();
    d.fn.indySites.push_back(std::move(site));

    auto instr  = std::make_unique<SSAIntrinsicInstruction>();
    instr->name = "indy:" + std::to_string(siteIndex);
    instr->args.resize(argTypes.size(), nullptr);
    for (int i = static_cast<int>(argTypes.size()) - 1; i >= 0; --i) {
        SSAValue* arg = d.fs.pop();
        if (!arg) {
            reason = "stack underflow at INVOKE_DYNAMIC args";
            return false;
        }
        instr->args[static_cast<size_t>(i)] = arg;
    }
    instr->effect                  = SSAEffect::externalCall();
    instr->effect.resolvesExternal = true;
    if (retType != SSAType::VOID) {
        instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), retType);
        instr->hasResult    = true;
        SSAValue* resultPtr = &instr->result;
        d.sb->instructions.push_back(std::move(instr));
        d.fs.push(resultPtr);
    }
    else {
        d.sb->instructions.push_back(std::move(instr));
    }
    return true;
}

bool lowerNewObject(const MIRNewObject* no, LowerCtx& d, std::string& /*reason*/)
{
    auto instr                    = std::make_unique<SSAAllocateInstruction>();
    instr->allocationKind         = SSAAllocationKind::Object;
    instr->typeDesc               = no->typeDesc;
    instr->result                 = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::REF);
    instr->hasResult              = true;
    instr->effect                 = SSAEffect::externalCall();
    instr->effect.resolvesExternal = true;
    SSAValue* resultPtr            = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerNewArray(const MIRNewArray* na, LowerCtx& d, std::string& reason)
{
    SSAValue* count = d.fs.pop();
    if (!count) {
        reason = "stack underflow at NEW_ARRAY";
        return false;
    }
    auto instr            = std::make_unique<SSAAllocateInstruction>();
    instr->allocationKind = SSAAllocationKind::Array;
    instr->typeDesc       = d.arena.intern(ca::str::Utf8StringRef::from_string_view(arrayDescFor(na)));
    instr->args.push_back(count);
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::REF);
    instr->hasResult = true;
    instr->effect    = SSAEffect::externalCall();
    instr->effect.resolvesExternal = true;
    SSAValue* resultPtr            = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerMultiNewArray(const MIRMultiNewArray* ma, LowerCtx& d, std::string& reason)
{
    if (ma->dims <= 0) {
        reason = "invalid MULTI_NEW_ARRAY dims";
        return false;
    }
    auto instr            = std::make_unique<SSAAllocateInstruction>();
    instr->allocationKind = SSAAllocationKind::MultiArray;
    instr->typeDesc       = ma->typeDesc;
    instr->args.resize(static_cast<size_t>(ma->dims), nullptr);
    for (int i = ma->dims - 1; i >= 0; --i) {
        SSAValue* dim = d.fs.pop();
        if (!dim) {
            reason = "stack underflow at MULTI_NEW_ARRAY";
            return false;
        }
        instr->args[static_cast<size_t>(i)] = dim;
    }
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::REF);
    instr->hasResult = true;
    instr->effect    = SSAEffect::externalCall();
    instr->effect.resolvesExternal = true;
    SSAValue* resultPtr            = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

// ---- 分区 7：数组与类型检查（ARRAY_LENGTH / ARRAY_LOAD / ARRAY_STORE / CHECK_CAST / INSTANCE_OF / MONITOR_*） ----

bool lowerArrayLength(LowerCtx& d, std::string& reason)
{
    SSAValue* array = d.fs.pop();
    if (!array) {
        reason = "stack underflow at ARRAY_LENGTH";
        return false;
    }
    auto instr  = std::make_unique<SSAIntrinsicInstruction>();
    instr->name = "array.length";
    instr->args.push_back(array);
    instr->result             = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::I32);
    instr->hasResult          = true;
    instr->effect.mayThrow    = true;
    instr->effect.readsMemory = true;
    SSAValue* resultPtr       = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerArrayLoad(const MIRArrayLoad* al, LowerCtx& d, std::string& reason)
{
    SSAValue* index = d.fs.pop();
    SSAValue* array = d.fs.pop();
    if (!array || !index) {
        reason = "stack underflow at ARRAY_LOAD";
        return false;
    }
    auto instr          = std::make_unique<SSAArrayLoadInstruction>();
    instr->array        = array;
    instr->index        = index;
    instr->elementType  = mirKindToSSA(al->componentType);
    instr->result       = SSAInstructionResult(d.fn.ids.newValueId(), instr->elementType);
    instr->hasResult    = true;
    instr->effect       = SSAEffect::readMemory().withMayThrow();
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerArrayStore(const MIRArrayStore* as, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    SSAValue* index = d.fs.pop();
    SSAValue* array = d.fs.pop();
    if (!array || !index || !value) {
        reason = "stack underflow at ARRAY_STORE";
        return false;
    }
    auto instr         = std::make_unique<SSAArrayStoreInstruction>();
    instr->array       = array;
    instr->index       = index;
    instr->value       = value;
    instr->elementType = mirKindToSSA(as->componentType);
    instr->effect      = SSAEffect::writeMemory().withMayThrow();
    d.sb->instructions.push_back(std::move(instr));
    return true;
}

bool lowerCheckCast(const MIRCheckCast* cc, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    if (!value) {
        reason = "stack underflow at CHECK_CAST";
        return false;
    }
    auto instr               = std::make_unique<SSAConvertInstruction>();
    instr->convertKind       = SSAConvertKind::ReferenceCast;
    instr->value             = value;
    instr->targetType        = SSAType::REF;
    instr->referenceTypeDesc = cc->typeDesc;
    instr->result            = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::REF);
    instr->hasResult         = true;
    markRuntimeCheckEffect(instr.get());
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerInstanceOf(const MIRInstanceOf* io, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    if (!value) {
        reason = "stack underflow at INSTANCE_OF";
        return false;
    }
    auto instr  = std::make_unique<SSAIntrinsicInstruction>();
    instr->name = "instanceof:" + io->typeDesc.to_std_string();
    instr->args.push_back(value);
    instr->result    = SSAInstructionResult(d.fn.ids.newValueId(), SSAType::I32);
    instr->hasResult = true;
    markRuntimeCheckEffect(instr.get());
    SSAValue* resultPtr = &instr->result;
    d.sb->instructions.push_back(std::move(instr));
    d.fs.push(resultPtr);
    return true;
}

bool lowerMonitor(const MIRMonitor* mon, LowerCtx& d, std::string& reason)
{
    SSAValue* value = d.fs.pop();
    if (!value) {
        reason = "stack underflow at MONITOR";
        return false;
    }
    auto instr  = std::make_unique<SSAIntrinsicInstruction>();
    instr->name = mon->enter ? "monitor.enter" : "monitor.exit";
    instr->args.push_back(value);
    instr->effect                     = SSAEffect::barrier();
    instr->effect.mayThrow            = true;
    instr->effect.readsExternalState  = true;
    instr->effect.writesExternalState = true;
    d.sb->instructions.push_back(std::move(instr));
    return true;
}

}   // namespace

std::vector<ExceptionHandler> SSABuilder::exceptionTable(const std::vector<Block*>& blocks) const
{
    if (blocks.empty()) return {};
    return blocks.front()->exceptionTable;
}

void SSABuilder::collectOutEdges(Block* b, std::vector<int>& targets)
{
    for (auto& node : b->nodes) {
        switch (node->kind) {
        case MIRKind::UNCOND_BRANCH:
            targets.push_back(mirLabelKey(ca::core::tag_cast::cast<MIRUncondBranch>(node.get())->targetLabel));
            break;
        case MIRKind::BRANCH:
            targets.push_back(mirLabelKey(ca::core::tag_cast::cast<MIRBranch>(node.get())->targetLabel));
            break;
        case MIRKind::BRANCH_ZERO:
            targets.push_back(mirLabelKey(ca::core::tag_cast::cast<MIRBranchZero>(node.get())->targetLabel));
            break;
        case MIRKind::SWITCH:
        {
            auto* sw = ca::core::tag_cast::cast<MIRSwitch>(node.get());
            targets.push_back(mirLabelKey(sw->defaultLabel));
            for (auto& c : sw->cases) targets.push_back(mirLabelKey(c.targetLabel));
            break;
        }
        default: break;
        }
    }
}

// 块是否以**无 fallthrough** 的终结指令结尾（goto/return/throw/switch）。
// 条件分支（BRANCH/BRANCH_ZERO）有 fallthrough 后继，不算"无 fallthrough" ——
// 它们同时有跳转目标和 fallthrough 两个后继，仍需加 fallthrough 边。
static bool blockEndsWithTerminator(Block* b)
{
    for (auto it = b->nodes.rbegin(); it != b->nodes.rend(); ++it) {
        MIRKind k = (*it)->kind;
        if (k == MIRKind::LABEL || k == MIRKind::FRAME || k == MIRKind::LINE || k == MIRKind::NOP)
            continue;
        // 只有无条件转移（GOTO/SWITCH）和返回/抛出才无 fallthrough。
        return k == MIRKind::UNCOND_BRANCH || k == MIRKind::SWITCH || k == MIRKind::RETURN ||
               k == MIRKind::RETURN_VALUE || k == MIRKind::THROW;
    }
    return false;
}

// ---- 阶段 A：构造 reaching-def 视图 ----
std::vector<ReachingDefBlock> SSABuilder::buildReachingDefView(const std::vector<Block*>& blocks)
{
    // label -> 所有以它为目标 label 的源块（前驱）
    std::unordered_map<int, std::vector<int>> predsOf;
    const auto                                handlers = exceptionTable(blocks);
    for (size_t i = 0; i < blocks.size(); ++i) {
        Block*           b = blocks[i];
        std::vector<int> outs;
        collectOutEdges(b, outs);
        // 非终结块 fallthrough 到序列下一块（R1a.2：CFG 保真的关键 —— fallthrough 也是边）。
        if (!blockEndsWithTerminator(b) && i + 1 < blocks.size()) {
            outs.push_back(blocks[i + 1]->id);
        }
        for (int t : outs) predsOf[t].push_back(b->id);

        for (const auto& handler : handlers) {
            if (blockInProtectedRange(blocks, handler, b)) {
                auto* handlerBlock = findBlockByLabel(blocks, handler.handlerLabel);
                if (handlerBlock) predsOf[handlerBlock->id].push_back(b->id);
            }
        }
    }

    std::vector<ReachingDefBlock> view;
    view.reserve(blocks.size());
    for (auto* b : blocks) {
        ReachingDefBlock rdb;
        rdb.labelId = b->id;
        auto pit    = predsOf.find(b->id);
        if (pit != predsOf.end()) rdb.predecessorLabels = pit->second;

        // 块内 def：扫 MIRLocalStore / MIRIinc
        int idx = 0;
        for (auto& node : b->nodes) {
            if (node->kind == MIRKind::LOCAL_STORE) {
                rdb.localDefs.push_back({idx, ca::core::tag_cast::cast<MIRLocalStore>(node.get())->varIndex});
            }
            else if (node->kind == MIRKind::IINC) {
                rdb.localDefs.push_back({idx, ca::core::tag_cast::cast<MIRIinc>(node.get())->varIndex});
            }
            ++idx;
        }
        view.push_back(std::move(rdb));
    }
    return view;
}

// ---- 阶段 B：建 SSABlock[]，决定 args ----
void SSABuilder::buildBlocks(SSAFunction& fn, const std::vector<Block*>& l1blocks,
                             const ReachingDefResult& rd)
{
    const auto handlers = exceptionTable(l1blocks);
    for (size_t blockIndex = 0; blockIndex < l1blocks.size(); ++blockIndex) {
        auto* lb      = l1blocks[blockIndex];
        auto  sb      = std::make_unique<SSABlock>(fn.ids.newBlockId());
        sb->l1LabelId = lb->id;

        // args：所有 IN[B] 有 ≥1 def 的 local slot 都开 arg（R1 简化策略，见 ssa_algorithm.md §3.4）。
        auto it = rd.in.find(lb->id);
        if (it != rd.in.end()) {
            for (const auto& [slot, defs] : it->second.bySlot) {
                if (defs.empty()) continue;
                // 每个有 def 的 slot 开一个 arg（≥2 源 = 真 phi；1 源 = 冗余但正确）。
                // 类型从 reaching defs 回看具体 STORE/IINC，供 RegC 保留 long/double 槽语义。
                sb->args.emplace_back(fn.ids.newValueId(), blockArgType(l1blocks, defs), slot);
            }
        }

        const bool isHandler = isExceptionHandlerBlock(handlers, lb);
        if (!isHandler) {
            auto stackTypes = stackArgTypes(l1blocks, blockIndex);
            for (size_t i = 0; i < stackTypes.size(); ++i) {
                sb->args.emplace_back(
                    fn.ids.newValueId(), stackTypes[i], ssaStackValueSlot(static_cast<int>(i)));
            }
        }

        if (isHandler) {
            sb->args.emplace_back(fn.ids.newValueId(), SSAType::REF, kSSAExceptionObjectSlot);
        }

        fn.blocks.push_back(std::move(sb));
    }

    std::unordered_map<int, SSABlock*>          ssaByLabel;
    std::unordered_map<const Block*, SSABlock*> ssaByL1Block;
    for (size_t i = 0; i < fn.blocks.size() && i < l1blocks.size(); ++i) {
        ssaByLabel[fn.blocks[i]->l1LabelId] = fn.blocks[i].get();
        ssaByL1Block[l1blocks[i]]           = fn.blocks[i].get();
    }
    for (const auto& handler : handlers) {
        SSAExceptionRegion region;
        auto*              handlerBlock = findBlockByLabel(l1blocks, handler.handlerLabel);
        auto               handlerIt    = ssaByL1Block.find(handlerBlock);
        region.handler        = handlerIt != ssaByL1Block.end() ? handlerIt->second : nullptr;
        region.exceptionValue = findExceptionArg(region.handler);
        region.caughtType     = handler.catchType;
        for (auto* lb : l1blocks) {
            if (blockInProtectedRange(l1blocks, handler, lb)) {
                auto it = ssaByLabel.find(lb->id);
                if (it != ssaByLabel.end()) region.protectedBlocks.push_back(it->second);
            }
        }
        if (region.handler && region.exceptionValue && !region.protectedBlocks.empty()) {
            fn.exceptionRegions.push_back(std::move(region));
        }
    }
}

// ---- 阶段 C：逐块翻译 ----
bool SSABuilder::translateBlocks(SSAFunction& fn, const std::vector<Block*>& l1blocks,
                                 const ReachingDefResult& rd, int methodMaxLocals,
                                 std::string& reason)
{
    (void)rd;
    // l1Label -> SSABlock*
    std::unordered_map<int, SSABlock*> ssaByLabel;
    for (auto& sb : fn.blocks) ssaByLabel[sb->l1LabelId] = sb.get();

    // 每个 SSA 块的入口 FrameState 初始化：
    //   locals[slot]：若 slot 是该块的 arg → arg 指针；否则从方法入口参数或 reaching-def 兜底。
    // R1 简化：只处理"slot 是 arg"的情况；非 arg 的 slot 引用 = opaque 兜底（测试样本不应触发）。

    for (auto* lb : l1blocks) {
        SSABlock*  sb = ssaByLabel.at(lb->id);
        FrameState fs(methodMaxLocals > 0 ? methodMaxLocals : 1);
        if (fs.locals.size() < 8) fs.locals.resize(8, nullptr);

        // Method parameters are available in every block unless a block arg
        // shadows the same local slot.  This keeps loop conditions that read an
        // unchanged parameter from turning into opaque undef_local values.
        for (auto& p : fn.parameters) {
            if (p->index < fs.locals.size()) fs.locals[p->index] = p.get();
        }

        // 块入口：把 args 绑到对应 slot；若参数 slot 被前驱写过，arg 覆盖原参数。
        for (auto& arg : sb->args) {
            if (arg.slot >= 0 && arg.slot < static_cast<int>(fs.locals.size())) {
                fs.locals[arg.slot] = &arg;
            }
            else if (isSSAStackValueSlot(arg.slot)) {
                // StackMapTable 的 stack 数组是 bottom-to-top 顺序，FrameState 保持同序，
                // 后续 pop() 仍能从 vector 尾部取得 JVM 栈顶。
                fs.push(&arg);
            }
        }
        if (auto* exceptionArg = findExceptionArg(sb)) {
            fs.push(exceptionArg);
        }

        // 翻译块内节点
        for (auto& node : lb->nodes) {
            // lowering 上下文：所有 per-kind lower 函数通过它读写 SSA 输出与栈模拟。
            LowerCtx d{fn, sb, fs, ssaByLabel, l1blocks, lb, *arena_};
            switch (node->kind) {
            case MIRKind::CONSTANT:
                if (!lowerConstant(ca::core::tag_cast::cast<MIRConstant>(node.get()), d, reason)) return false;
                break;
            case MIRKind::LOCAL_LOAD:
                if (!lowerLocalLoad(ca::core::tag_cast::cast<MIRLocalLoad>(node.get()), d, reason)) return false;
                break;
            case MIRKind::LOCAL_STORE:
                if (!lowerLocalStore(ca::core::tag_cast::cast<MIRLocalStore>(node.get()), d, reason)) return false;
                break;
            case MIRKind::IINC:
                if (!lowerIinc(ca::core::tag_cast::cast<MIRIinc>(node.get()), d, reason)) return false;
                break;
            case MIRKind::BINARY_OP:
                if (!lowerBinaryOp(ca::core::tag_cast::cast<MIRBinaryOp>(node.get()), d, reason)) return false;
                break;
            case MIRKind::UNARY_OP:
                if (!lowerUnaryOp(ca::core::tag_cast::cast<MIRUnaryOp>(node.get()), d, reason)) return false;
                break;
            case MIRKind::CAST:
                if (!lowerCast(ca::core::tag_cast::cast<MIRCast>(node.get()), d, reason)) return false;
                break;
            case MIRKind::CMP:
                if (!lowerCmp(ca::core::tag_cast::cast<MIRCmp>(node.get()), d, reason)) return false;
                break;
            // 栈操作：只动 FrameState，不产 IR（消除 cstack 锚点的核心）
            case MIRKind::STACK_POP:
                if (!lowerStackPop(d, reason)) return false;
                break;
            case MIRKind::STACK_POP2:
                if (!lowerStackPop2(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP:
                if (!lowerStackDup(d, reason)) return false;
                break;
            case MIRKind::STACK_SWAP:
                if (!lowerStackSwap(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP_X1:
                if (!lowerStackDupX1(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP_X2:
                if (!lowerStackDupX2(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP2:
                if (!lowerStackDup2(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP2_X1:
                if (!lowerStackDup2X1(d, reason)) return false;
                break;
            case MIRKind::STACK_DUP2_X2:
                if (!lowerStackDup2X2(d, reason)) return false;
                break;
            case MIRKind::RETURN:
                if (!lowerReturn(d, reason)) return false;
                break;
            case MIRKind::RETURN_VALUE:
                if (!lowerReturnValue(ca::core::tag_cast::cast<MIRReturnValue>(node.get()), d, reason)) return false;
                break;
            case MIRKind::UNCOND_BRANCH:
                if (!lowerUncondBranch(ca::core::tag_cast::cast<MIRUncondBranch>(node.get()), d, reason)) return false;
                break;
            case MIRKind::BRANCH_ZERO:
                if (!lowerBranchZero(ca::core::tag_cast::cast<MIRBranchZero>(node.get()), d, reason)) return false;
                break;
            case MIRKind::BRANCH:
                if (!lowerBranch(ca::core::tag_cast::cast<MIRBranch>(node.get()), d, reason)) return false;
                break;
            case MIRKind::SWITCH:
                if (!lowerSwitch(ca::core::tag_cast::cast<MIRSwitch>(node.get()), d, reason)) return false;
                break;
            case MIRKind::THROW:
                if (!lowerThrow(d, reason)) return false;
                break;
            case MIRKind::FIELD_GET:
                if (!lowerFieldGet(ca::core::tag_cast::cast<MIRFieldGet>(node.get()), d, reason)) return false;
                break;
            case MIRKind::FIELD_PUT:
                if (!lowerFieldPut(ca::core::tag_cast::cast<MIRFieldPut>(node.get()), d, reason)) return false;
                break;
            case MIRKind::STATIC_GET:
                if (!lowerStaticGet(ca::core::tag_cast::cast<MIRStaticGet>(node.get()), d, reason)) return false;
                break;
            case MIRKind::STATIC_PUT:
                if (!lowerStaticPut(ca::core::tag_cast::cast<MIRStaticPut>(node.get()), d, reason)) return false;
                break;
            case MIRKind::INVOKE:
                if (!lowerInvoke(ca::core::tag_cast::cast<MIRInvoke>(node.get()), d, reason)) return false;
                break;
            case MIRKind::NEW_OBJECT:
                if (!lowerNewObject(ca::core::tag_cast::cast<MIRNewObject>(node.get()), d, reason)) return false;
                break;
            case MIRKind::NEW_ARRAY:
                if (!lowerNewArray(ca::core::tag_cast::cast<MIRNewArray>(node.get()), d, reason)) return false;
                break;
            case MIRKind::MULTI_NEW_ARRAY:
                if (!lowerMultiNewArray(ca::core::tag_cast::cast<MIRMultiNewArray>(node.get()), d, reason)) return false;
                break;
            case MIRKind::ARRAY_LENGTH:
                if (!lowerArrayLength(d, reason)) return false;
                break;
            case MIRKind::ARRAY_LOAD:
                if (!lowerArrayLoad(ca::core::tag_cast::cast<MIRArrayLoad>(node.get()), d, reason)) return false;
                break;
            case MIRKind::ARRAY_STORE:
                if (!lowerArrayStore(ca::core::tag_cast::cast<MIRArrayStore>(node.get()), d, reason)) return false;
                break;
            case MIRKind::CHECK_CAST:
                if (!lowerCheckCast(ca::core::tag_cast::cast<MIRCheckCast>(node.get()), d, reason)) return false;
                break;
            case MIRKind::INSTANCE_OF:
                if (!lowerInstanceOf(ca::core::tag_cast::cast<MIRInstanceOf>(node.get()), d, reason)) return false;
                break;
            case MIRKind::MONITOR_ENTER:
                if (!lowerMonitor(ca::core::tag_cast::cast<MIRMonitor>(node.get()), d, reason)) return false;
                break;
            case MIRKind::NOP:
            case MIRKind::LABEL:
            case MIRKind::FRAME:
            case MIRKind::LINE:
                // NOP/元数据：忽略。
                break;
            case MIRKind::INVOKE_DYNAMIC:
                if (!lowerInvokeDynamic(ca::core::tag_cast::cast<MIRInvokeDynamic>(node.get()), d, reason))
                    return false;
                break;
            }
        }

        // 块翻译完若仍 Unreachable（无显式 terminator）= fallthrough 块：
        // 合成 SSAJump 到序列下一块，并按后继 args 传值。
        if (std::holds_alternative<SSAUnreachableTerminator>(sb->terminator)) {
            SSABlock*         nextSb = fallthroughBlock(l1blocks, ssaByLabel, lb);
            SSAJumpTerminator jt;
            jt.target      = buildSuccessor(nextSb, fs, fn);
            sb->terminator = std::move(jt);
        }

        // Exception-table entries are implicit CFG edges. Preserve the
        // handler's local arguments using the frame that is live at this
        // protected block; the synthetic exception stack argument is assigned
        // by RegC from the pending JNI exception and is therefore omitted.
        for (auto& region : fn.exceptionRegions) {
            if (std::find(region.protectedBlocks.begin(), region.protectedBlocks.end(), sb) ==
                region.protectedBlocks.end())
                continue;

            SSAExceptionEdge edge;
            edge.protectedBlock = sb;
            for (auto& arg : region.handler->args) {
                if (&arg == region.exceptionValue) continue;
                SSAValue* value = nullptr;
                if (arg.slot >= 0 && arg.slot < static_cast<int>(fs.locals.size())) {
                    value = fs.locals[arg.slot];
                }
                if (!value) value = fn.newOpaque(arg.type, "undef_exception_local");
                edge.handlerArgs.push_back(value);
            }
            region.edges.push_back(std::move(edge));
        }
    }

    // 建 entry 合成块：Jump 到真入口（L1 第一个块）。
    auto              entryBlock = std::make_unique<SSABlock>(fn.ids.newBlockId());
    SSABlock*         realEntry  = fn.blocks.empty() ? nullptr : fn.blocks[0].get();
    SSAJumpTerminator jt;
    jt.target.block = realEntry;
    // 传方法参数给真入口的 args（若真入口 args 对应参数 slot）
    if (realEntry) {
        for (auto& arg : realEntry->args) {
            // 方法参数 slot 0..N 对应 SSAParameter
            bool bound = false;
            for (auto& p : fn.parameters) {
                if (static_cast<int>(p->index) == arg.slot) {
                    jt.target.args.push_back(p.get());
                    bound = true;
                    break;
                }
            }
            if (!bound && isSSAStackValueSlot(arg.slot)) {
                jt.target.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "entry_stack_arg_unbound"));
            }
            else if (!bound) {
                jt.target.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "entry_arg_unbound"));
            }
        }
    }
    entryBlock->terminator = std::move(jt);
    fn.entry               = entryBlock.get();
    fn.blocks.insert(fn.blocks.begin(), std::move(entryBlock));

    return true;
}

// ---- 入口 ----
std::unique_ptr<SSAFunction> SSABuilder::build(const std::vector<Block*>& blocks,
                                               const std::string&         ownerName,
                                               const std::string&         methodName,
                                               const std::string&         methodDesc, bool isStatic,
                                               int                       methodMaxLocals,
                                               ca::str::Utf8StringArena& arena,
                                               std::string&              reason)
{
    (void)methodMaxLocals;
    arena_ = &arena;
    if (blocks.empty()) {
        reason = "no blocks";
        return nullptr;
    }

    auto fn          = std::make_unique<SSAFunction>();
    fn->symbol.owner = ownerName;
    fn->symbol.name  = methodName;
    fn->symbol.desc  = methodDesc;

    ca::str::Utf8StringRef descRef    = ca::str::Utf8StringRef::from_cstr(methodDesc.c_str());
    auto                   methodType = Type::getMethodType(descRef);
    fn->symbol.returnType             = jvmTypeToSSA(Type::getReturnType(descRef));

    uint32_t localSlot = 0;
    if (!isStatic) {
        auto thisParam =
            std::make_unique<SSAParameter>(fn->ids.newValueId(), SSAType::REF, localSlot);
        fn->parameters.push_back(std::move(thisParam));
        localSlot += 1;
    }
    for (const auto& argType : methodType.getArgumentTypes()) {
        SSAType paramType = jvmTypeToSSA(argType);
        fn->symbol.parameterTypes.push_back(paramType);
        auto p = std::make_unique<SSAParameter>(fn->ids.newValueId(), paramType, localSlot);
        fn->parameters.push_back(std::move(p));
        localSlot += static_cast<uint32_t>(ssaStackSize(paramType));
    }

    // 阶段 A
    auto rdView = buildReachingDefView(blocks);
    auto rd     = computeReachingDef(rdView);

    // 阶段 B
    buildBlocks(*fn, blocks, rd);

    // 阶段 C
    if (!translateBlocks(*fn, blocks, rd, methodMaxLocals, reason)) {
        return nullptr;
    }

    return fn;
}

}   // namespace mj2x

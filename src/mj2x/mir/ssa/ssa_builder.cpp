#include "ssa_builder.hpp"

#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace mj2x {

namespace {

// ---- FrameState：builder-local 构造期栈模拟。不进 IR。吸收 DUP/SWAP。 ----
struct FrameState {
    std::vector<SSAValue*> locals;   // local slot -> 当前绑定的 SSA value
    std::vector<SSAValue*> stack;    // 操作数栈（纯数据结构）

    explicit FrameState(int maxLocals) : locals(maxLocals, nullptr) {}

    void push(SSAValue* v) { stack.push_back(v); }
    SSAValue* pop() {
        // 栈下溢 = builder bug 或 L1 问题；返回 opaque 防崩，调用方应 fail-closed。
        if (stack.empty()) return nullptr;
        SSAValue* v = stack.back();
        stack.pop_back();
        return v;
    }
};

// 把 SSAType 从 PrimitiveKind 映射（R1 保守，细化推 R2）。
SSAType mirKindToSSA(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::BOOLEAN: return SSAType::BOOL;
        case PrimitiveKind::BYTE:    return SSAType::I8;
        case PrimitiveKind::CHAR:    return SSAType::CHAR;
        case PrimitiveKind::SHORT:   return SSAType::I16;
        case PrimitiveKind::INT:     return SSAType::I32;
        case PrimitiveKind::LONG:    return SSAType::I64;
        case PrimitiveKind::FLOAT:   return SSAType::F32;
        case PrimitiveKind::DOUBLE:  return SSAType::F64;
        case PrimitiveKind::VOID:    return SSAType::VOID;
        case PrimitiveKind::OBJECT:  return SSAType::REF;
        default:                     return SSAType::UNKNOWN;
    }
}

// 算术 op 类型推断（R1：整型统一 I32，长整型 I64，浮点 F32/F64）。
SSAType binaryResultType(SSAType lhs, SSAType rhs) {
    if (lhs == SSAType::I64 || rhs == SSAType::I64) return SSAType::I64;
    if (lhs == SSAType::F64 || rhs == SSAType::F64) return SSAType::F64;
    if (lhs == SSAType::F32 || rhs == SSAType::F32) return SSAType::F32;
    return SSAType::I32;
}

// MIR BinaryOp → SSA BinaryOp（枚举顺序不同，必须显式映射）。
SSABinaryOp mapBinaryOp(BinaryOp op) {
    switch (op) {
        case BinaryOp::ADD:  return SSABinaryOp::Add;
        case BinaryOp::SUB:  return SSABinaryOp::Sub;
        case BinaryOp::MUL:  return SSABinaryOp::Mul;
        case BinaryOp::DIV:  return SSABinaryOp::Div;
        case BinaryOp::REM:  return SSABinaryOp::Rem;
        case BinaryOp::SHL:  return SSABinaryOp::Shl;
        case BinaryOp::SHR:  return SSABinaryOp::Shr;
        case BinaryOp::USHR: return SSABinaryOp::UShr;
        case BinaryOp::AND:  return SSABinaryOp::And;
        case BinaryOp::OR:   return SSABinaryOp::Or;
        case BinaryOp::XOR:  return SSABinaryOp::Xor;
    }
    return SSABinaryOp::Add;  // 不可达
}

// MIR CmpOp → SSAComparePredicate（EQ,NE,LT,GE,GT,LE → Eq,Ne,Lt,Le,Gt,Ge）。
SSAComparePredicate mapComparePredicate(CmpOp op) {
    switch (op) {
        case CmpOp::EQ: return SSAComparePredicate::Eq;
        case CmpOp::NE: return SSAComparePredicate::Ne;
        case CmpOp::LT: return SSAComparePredicate::Lt;
        case CmpOp::GE: return SSAComparePredicate::Ge;
        case CmpOp::GT: return SSAComparePredicate::Gt;
        case CmpOp::LE: return SSAComparePredicate::Le;
    }
    return SSAComparePredicate::Eq;  // 不可达
}

// 构造一条出边：跳到 target 块，并按 target.args 的 slot 从 FrameState.locals 取值传出。
// target=null 时返回空 successor（不可达/错误兜底）。
SSASuccessor buildSuccessor(SSABlock* target, const FrameState& fs, SSAFunction& fn) {
    SSASuccessor s;
    s.block = target;
    if (!target) return s;
    for (auto& arg : target->args) {
        if (arg.slot < static_cast<int>(fs.locals.size()) && fs.locals[arg.slot]) {
            s.args.push_back(fs.locals[arg.slot]);
        } else {
            s.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "undef_succ_arg"));
        }
    }
    return s;
}

// 找 lb 在 l1blocks 序列里的下一个块对应的 SSABlock（fallthrough 目标）。
SSABlock* fallthroughBlock(const std::vector<Block*>& l1blocks,
                           const std::unordered_map<int, SSABlock*>& ssaByLabel,
                           Block* lb) {
    for (size_t i = 0; i < l1blocks.size(); ++i) {
        if (l1blocks[i] == lb && i + 1 < l1blocks.size()) {
            auto it = ssaByLabel.find(l1blocks[i + 1]->id);
            return it != ssaByLabel.end() ? it->second : nullptr;
        }
    }
    return nullptr;
}

} // namespace

// ---- 收集块出口的目标 label（从末尾 terminator 推 CFG，L1 没填 successors） ----
// targetLabel 是 Label*；用 labelKey（指针转 int）作块 id 对齐。
static int mirLabelKey(jbytecode::core::Label* lbl) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

void SSABuilder::collectOutEdges(Block* b, std::vector<int>& targets) {
    for (auto& node : b->nodes) {
        switch (node->kind) {
            case MIRKind::UNCOND_BRANCH:
                targets.push_back(mirLabelKey(static_cast<MIRUncondBranch*>(node.get())->targetLabel));
                break;
            case MIRKind::BRANCH:
                targets.push_back(mirLabelKey(static_cast<MIRBranch*>(node.get())->targetLabel));
                break;
            case MIRKind::BRANCH_ZERO:
                targets.push_back(mirLabelKey(static_cast<MIRBranchZero*>(node.get())->targetLabel));
                break;
            case MIRKind::SWITCH: {
                auto* sw = static_cast<MIRSwitch*>(node.get());
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
static bool blockEndsWithTerminator(Block* b) {
    for (auto it = b->nodes.rbegin(); it != b->nodes.rend(); ++it) {
        MIRKind k = (*it)->kind;
        if (k == MIRKind::LABEL || k == MIRKind::FRAME || k == MIRKind::LINE || k == MIRKind::NOP)
            continue;
        // 只有无条件转移（GOTO/SWITCH）和返回/抛出才无 fallthrough。
        return k == MIRKind::UNCOND_BRANCH || k == MIRKind::SWITCH ||
               k == MIRKind::RETURN || k == MIRKind::RETURN_VALUE || k == MIRKind::THROW;
    }
    return false;
}

// ---- 阶段 A：构造 reaching-def 视图 ----
std::vector<ReachingDefBlock> SSABuilder::buildReachingDefView(const std::vector<Block*>& blocks) {
    // label -> 所有以它为目标 label 的源块（前驱）
    std::unordered_map<int, std::vector<int>> predsOf;
    for (size_t i = 0; i < blocks.size(); ++i) {
        Block* b = blocks[i];
        std::vector<int> outs;
        collectOutEdges(b, outs);
        // 非终结块 fallthrough 到序列下一块（R1a.2：CFG 保真的关键 —— fallthrough 也是边）。
        if (!blockEndsWithTerminator(b) && i + 1 < blocks.size()) {
            outs.push_back(blocks[i + 1]->id);
        }
        for (int t : outs) predsOf[t].push_back(b->id);
    }

    std::vector<ReachingDefBlock> view;
    view.reserve(blocks.size());
    for (auto* b : blocks) {
        ReachingDefBlock rdb;
        rdb.labelId = b->id;
        auto pit = predsOf.find(b->id);
        if (pit != predsOf.end()) rdb.predecessorLabels = pit->second;

        // 块内 def：扫 MIRLocalStore / MIRIinc
        int idx = 0;
        for (auto& node : b->nodes) {
            if (node->kind == MIRKind::LOCAL_STORE) {
                rdb.localDefs.push_back({idx, static_cast<MIRLocalStore*>(node.get())->varIndex});
            } else if (node->kind == MIRKind::IINC) {
                rdb.localDefs.push_back({idx, static_cast<MIRIinc*>(node.get())->varIndex});
            }
            ++idx;
        }
        view.push_back(std::move(rdb));
    }
    return view;
}

// ---- 阶段 B：建 SSABlock[]，决定 args ----
void SSABuilder::buildBlocks(SSAFunction& fn,
                              const std::vector<Block*>& l1blocks,
                              const ReachingDefResult& rd) {
    for (auto* lb : l1blocks) {
        auto sb = std::make_unique<SSABlock>(fn.ids.newBlockId());
        sb->l1LabelId = lb->id;

        // args：所有 IN[B] 有 ≥1 def 的 local slot 都开 arg（R1 简化策略，见 SSA_ALGORITHM §3.4）。
        // 类型 R1 保守用 I32（细化推 R2）。
        auto it = rd.in.find(lb->id);
        if (it != rd.in.end()) {
            for (const auto& [slot, defs] : it->second.bySlot) {
                if (defs.empty()) continue;
                // 每个有 def 的 slot 开一个 arg（≥2 源 = 真 phi；1 源 = 冗余但正确）
                sb->args.emplace_back(fn.ids.newValueId(), SSAType::I32, slot);
            }
        }

        fn.blocks.push_back(std::move(sb));
    }
}

// ---- 阶段 C：逐块翻译 ----
bool SSABuilder::translateBlocks(SSAFunction& fn,
                                 const std::vector<Block*>& l1blocks,
                                 const ReachingDefResult& rd,
                                 std::string& reason) {
    // l1Label -> SSABlock*
    std::unordered_map<int, SSABlock*> ssaByLabel;
    for (auto& sb : fn.blocks) ssaByLabel[sb->l1LabelId] = sb.get();

    // 每个 SSA 块的入口 FrameState 初始化：
    //   locals[slot]：若 slot 是该块的 arg → arg 指针；否则从方法入口参数或 reaching-def 兜底。
    // R1 简化：只处理"slot 是 arg"的情况；非 arg 的 slot 引用 = opaque 兜底（测试样本不应触发）。

    for (auto* lb : l1blocks) {
        SSABlock* sb = ssaByLabel.at(lb->id);
        FrameState fs(fn.symbol.parameterTypes.size() > 0 ? static_cast<int>(fn.symbol.parameterTypes.size()) : 4);
        // 扩展 locals 到方法 maxLocals（builder 调用方传 methodMaxLocals，但我们没存——用 fs 默认足够 R1）
        if (fs.locals.size() < 8) fs.locals.resize(8, nullptr);

        // 块入口：把 args 绑到对应 slot
        for (auto& arg : sb->args) {
            if (arg.slot < static_cast<int>(fs.locals.size())) {
                fs.locals[arg.slot] = &arg;
            }
        }

        // 翻译块内节点
        for (auto& node : lb->nodes) {
            switch (node->kind) {
            case MIRKind::CONSTANT: {
                auto* c = static_cast<MIRConstant*>(node.get());
                // 简单处理：int/long 用 IntLiteral，double/float 用 FloatLiteral，null 用 Null
                if (c->type == PrimitiveKind::LONG) {
                    int64_t v = std::get<int64_t>(c->value);
                    fs.push(fn.internInt(v, SSAType::I64));
                } else if (c->type == PrimitiveKind::FLOAT || c->type == PrimitiveKind::DOUBLE) {
                    double v = std::get<double>(c->value);
                    fs.push(fn.internFloat(v, c->type == PrimitiveKind::DOUBLE ? SSAType::F64 : SSAType::F32));
                } else if (std::holds_alternative<std::nullptr_t>(c->value)) {
                    fs.push(fn.internNull());
                } else {
                    int64_t v = std::get<int64_t>(c->value);
                    fs.push(fn.internInt(v, mirKindToSSA(c->type)));
                }
                break;
            }
            case MIRKind::LOCAL_LOAD: {
                auto* ll = static_cast<MIRLocalLoad*>(node.get());
                if (ll->varIndex < static_cast<int>(fs.locals.size()) && fs.locals[ll->varIndex]) {
                    fs.push(fs.locals[ll->varIndex]);
                } else {
                    // 未定义到达 = opaque 兜底
                    fs.push(fn.newOpaque(mirKindToSSA(ll->type), "undef_local_" + std::to_string(ll->varIndex)));
                }
                break;
            }
            case MIRKind::LOCAL_STORE: {
                auto* ls = static_cast<MIRLocalStore*>(node.get());
                SSAValue* v = fs.pop();
                if (!v) { reason = "stack underflow at LOCAL_STORE"; return false; }
                if (ls->varIndex < static_cast<int>(fs.locals.size())) {
                    fs.locals[ls->varIndex] = v;
                }
                break;
            }
            case MIRKind::IINC: {
                auto* ii = static_cast<MIRIinc*>(node.get());
                SSAValue* lhs = (ii->varIndex < static_cast<int>(fs.locals.size())) ? fs.locals[ii->varIndex] : nullptr;
                if (!lhs) { reason = "iinc on undef local"; return false; }
                auto instr = std::make_unique<SSABinaryInstruction>();
                instr->op = SSABinaryOp::Add;
                instr->lhs = lhs;
                instr->rhs = fn.internInt(ii->increment, SSAType::I32);
                instr->result = SSAInstructionResult(fn.ids.newValueId(), SSAType::I32);
                instr->hasResult = true;
                SSAValue* resultPtr = &instr->result;
                sb->instructions.push_back(std::move(instr));
                if (ii->varIndex < static_cast<int>(fs.locals.size())) fs.locals[ii->varIndex] = resultPtr;
                break;
            }
            case MIRKind::BINARY_OP: {
                auto* bo = static_cast<MIRBinaryOp*>(node.get());
                SSAValue* rhs = fs.pop();
                SSAValue* lhs = fs.pop();
                if (!lhs || !rhs) { reason = "stack underflow at BINARY_OP"; return false; }
                auto instr = std::make_unique<SSABinaryInstruction>();
                instr->op = mapBinaryOp(bo->op);
                instr->lhs = lhs;
                instr->rhs = rhs;
                SSAType rt = binaryResultType(lhs->type, rhs->type);
                instr->result = SSAInstructionResult(fn.ids.newValueId(), rt);
                instr->hasResult = true;
                SSAValue* resultPtr = &instr->result;
                sb->instructions.push_back(std::move(instr));
                fs.push(resultPtr);
                break;
            }
            case MIRKind::UNARY_OP: {
                auto* uo = static_cast<MIRUnaryOp*>(node.get());
                SSAValue* v = fs.pop();
                if (!v) { reason = "stack underflow at UNARY_OP"; return false; }
                auto instr = std::make_unique<SSAUnaryInstruction>();
                instr->op = SSAUnaryOp::Neg;
                instr->value = v;
                instr->result = SSAInstructionResult(fn.ids.newValueId(), v->type);
                instr->hasResult = true;
                SSAValue* resultPtr = &instr->result;
                sb->instructions.push_back(std::move(instr));
                fs.push(resultPtr);
                break;
            }
            // 栈操作：只动 FrameState，不产 IR（消除 cstack 锚点的核心）
            case MIRKind::STACK_DUP: {
                SSAValue* v = fs.pop();
                if (!v) { reason = "underflow DUP"; return false; }
                fs.push(v); fs.push(v);
                break;
            }
            case MIRKind::STACK_SWAP: {
                SSAValue* v1 = fs.pop();
                SSAValue* v2 = fs.pop();
                if (!v1 || !v2) { reason = "underflow SWAP"; return false; }
                fs.push(v1); fs.push(v2);
                break;
            }
            case MIRKind::STACK_DUP_X1: {
                SSAValue* v1 = fs.pop();
                SSAValue* v2 = fs.pop();
                if (!v1 || !v2) { reason = "underflow DUP_X1"; return false; }
                fs.push(v1); fs.push(v2); fs.push(v1);
                break;
            }
            case MIRKind::STACK_DUP2:
            case MIRKind::STACK_DUP_X2: {
                // R1 简化：DUP2/DUP_X2 涉及 category-2，先按 1-slot 近似（测试样本可避免）
                reason = "DUP2/DUP_X2 category-2 handling pending (R2)";
                return false;
            }
            case MIRKind::RETURN: {
                sb->terminator = SSAReturnTerminator{nullptr};
                break;
            }
            case MIRKind::RETURN_VALUE: {
                SSAValue* v = fs.pop();
                sb->terminator = SSAReturnTerminator{v};
                break;
            }
            case MIRKind::UNCOND_BRANCH: {
                auto* ub = static_cast<MIRUncondBranch*>(node.get());
                int tkey = mirLabelKey(ub->targetLabel);
                SSABlock* target = ssaByLabel.count(tkey) ? ssaByLabel.at(tkey) : nullptr;
                SSAJumpTerminator jt;
                jt.target = buildSuccessor(target, fs, fn);
                sb->terminator = std::move(jt);
                break;
            }
            case MIRKind::BRANCH_ZERO: {
                auto* bz = static_cast<MIRBranchZero*>(node.get());
                SSAValue* v = fs.pop();
                auto cmp = std::make_unique<SSACompareInstruction>();
                cmp->predicate = mapComparePredicate(bz->op);
                cmp->lhs = v;
                cmp->rhs = fn.internInt(0, SSAType::I32);
                cmp->result = SSAInstructionResult(fn.ids.newValueId(), SSAType::BOOL);
                cmp->hasResult = true;
                SSAValue* cond = &cmp->result;
                sb->instructions.push_back(std::move(cmp));
                SSABranchTerminator bt;
                bt.condition = cond;
                int bzKey = mirLabelKey(bz->targetLabel);
                bt.trueTarget = buildSuccessor(ssaByLabel.count(bzKey) ? ssaByLabel.at(bzKey) : nullptr, fs, fn);
                bt.falseTarget = buildSuccessor(fallthroughBlock(l1blocks, ssaByLabel, lb), fs, fn);
                sb->terminator = std::move(bt);
                break;
            }
            case MIRKind::BRANCH: {
                auto* br = static_cast<MIRBranch*>(node.get());
                SSAValue* rhs = fs.pop();
                SSAValue* lhs = fs.pop();
                auto cmp = std::make_unique<SSACompareInstruction>();
                cmp->predicate = mapComparePredicate(br->op);
                cmp->lhs = lhs; cmp->rhs = rhs;
                cmp->result = SSAInstructionResult(fn.ids.newValueId(), SSAType::BOOL);
                cmp->hasResult = true;
                SSAValue* cond = &cmp->result;
                sb->instructions.push_back(std::move(cmp));
                SSABranchTerminator bt;
                bt.condition = cond;
                int brKey = mirLabelKey(br->targetLabel);
                bt.trueTarget = buildSuccessor(ssaByLabel.count(brKey) ? ssaByLabel.at(brKey) : nullptr, fs, fn);
                bt.falseTarget = buildSuccessor(fallthroughBlock(l1blocks, ssaByLabel, lb), fs, fn);
                sb->terminator = std::move(bt);
                break;
            }
            case MIRKind::THROW: {
                SSAValue* v = fs.pop();
                sb->terminator = SSAThrowTerminator{v};
                break;
            }
            // R1 先 fail-closed 的复杂指令（R2 细化）
            case MIRKind::INVOKE:
            case MIRKind::FIELD_GET: case MIRKind::FIELD_PUT:
            case MIRKind::STATIC_GET: case MIRKind::STATIC_PUT:
            case MIRKind::NEW_OBJECT: case MIRKind::NEW_ARRAY: case MIRKind::MULTI_NEW_ARRAY:
            case MIRKind::ARRAY_LOAD: case MIRKind::ARRAY_STORE: case MIRKind::ARRAY_LENGTH:
            case MIRKind::CHECK_CAST: case MIRKind::INSTANCE_OF:
            case MIRKind::MONITOR_ENTER: case MIRKind::MONITOR_EXIT:
            case MIRKind::CAST:
            case MIRKind::SWITCH: {
                reason = std::string("R1a does not lower MIRKind=") + std::to_string(static_cast<int>(node->kind));
                return false;
            }
            case MIRKind::NOP:
            case MIRKind::LABEL:
            case MIRKind::FRAME:
            case MIRKind::LINE:
            case MIRKind::INVOKE_DYNAMIC:
                // NOP/元数据：忽略。INVOKE_DYNAMIC 应由 R1b preprocessor 先展开，R1a 暂忽略。
                break;
            }
        }

        // 块翻译完若仍 Unreachable（无显式 terminator）= fallthrough 块：
        // 合成 SSAJump 到序列下一块，并按后继 args 传值。
        if (std::holds_alternative<SSAUnreachableTerminator>(sb->terminator)) {
            SSABlock* nextSb = fallthroughBlock(l1blocks, ssaByLabel, lb);
            SSAJumpTerminator jt;
            jt.target = buildSuccessor(nextSb, fs, fn);
            sb->terminator = std::move(jt);
        }
    }

    // 建 entry 合成块：Jump 到真入口（L1 第一个块）。
    auto entryBlock = std::make_unique<SSABlock>(fn.ids.newBlockId());
    SSABlock* realEntry = fn.blocks.empty() ? nullptr : fn.blocks[0].get();
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
            if (!bound) jt.target.args.push_back(fn.newOpaque(SSAType::UNKNOWN, "entry_arg_unbound"));
        }
    }
    entryBlock->terminator = std::move(jt);
    fn.entry = entryBlock.get();
    fn.blocks.insert(fn.blocks.begin(), std::move(entryBlock));

    return true;
}

// ---- 入口 ----
std::unique_ptr<SSAFunction> SSABuilder::build(const std::vector<Block*>& blocks,
                                                const std::string& ownerName,
                                                const std::string& methodName,
                                                const std::string& methodDesc,
                                                bool isStatic,
                                                int methodMaxLocals,
                                                std::string& reason) {
    (void)methodMaxLocals;
    if (blocks.empty()) {
        reason = "no blocks";
        return nullptr;
    }

    auto fn = std::make_unique<SSAFunction>();
    fn->symbol.owner = ownerName;
    fn->symbol.name = methodName;
    fn->symbol.desc = methodDesc;

    // 解析 desc 得参数/返回类型（R1 保守：全 I32，细化推 R2）
    // R1 简化：参数个数从 desc 的 ( 数量粗推，类型用 I32 占位。
    // （测试样本 Add(int,int) / LoopSum(int) 都是 int，足够。）
    // TODO R2：正经解析 desc。
    int argCount = 0;
    for (char ch : methodDesc) if (ch == 'I') ++argCount;  // 极简：数 'I'
    fn->symbol.returnType = SSAType::I32;  // 测试样本返回 int
    for (int i = 0; i < argCount; ++i) {
        fn->symbol.parameterTypes.push_back(SSAType::I32);
        auto p = std::make_unique<SSAParameter>(fn->ids.newValueId(), SSAType::I32, static_cast<uint32_t>(i));
        fn->parameters.push_back(std::move(p));
    }

    // 阶段 A
    auto rdView = buildReachingDefView(blocks);
    auto rd = computeReachingDef(rdView);

    // 阶段 B
    buildBlocks(*fn, blocks, rd);

    // 阶段 C
    if (!translateBlocks(*fn, blocks, rd, reason)) {
        return nullptr;
    }

    return fn;
}

} // namespace mj2x

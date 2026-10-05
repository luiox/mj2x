// mj2x SSA 层方法内联 pass。
// 详见 ssa_inline_passes.hpp。语义等价 + fail-closed：任一安全条件不满足
// 就跳过该调用点，绝不部分内联或破坏 CFG。
//
// 内联形状（单调用点）：
//   调用块: ... Call(args) ... -> 原后继
//   变为:
//   调用块: ... -> jump 克隆 entry
//   克隆体: entry -> ...（参数已绑定调用点实参）-> return 点改跳 merge
//   merge : 1 个 block arg（非 void）收集返回值; terminator = 调用块原 terminator
//   原 Call result 的所有引用重定向到 merge 的 block arg。

#include "mj2x/mir/passes/ssa_inline_passes.hpp"

#include "mj2x/mir/ssa/ssa_instruction.hpp"
#include "mj2x/mir/ssa/ssa_terminator.hpp"

#include <jbytecode/jbytecode.hpp>

#include <algorithm>
#include <unordered_map>

namespace mj2x {
namespace {

using jbytecode::core::Opcodes;

using ValueMap = std::unordered_map<const SSAValue*, SSAValue*>;

SSAValue* mapValue(SSAValue* v, const ValueMap& remap)
{
    if (v == nullptr) return nullptr;
    const auto it = remap.find(v);
    return it == remap.end() ? v : it->second;
}

/// 统计被调方法指令总数（大小门控）。
ca::usize countInstructions(const SSAFunction& fn)
{
    ca::usize count = 0;
    for (const auto& block : fn.blocks) count += block->instructions.size();
    return count;
}

/// 深拷贝一条指令；operand 全部经 remap 重映射；字符串字段重新入池
/// （被调方法的 SSA 在回调返回后析构，ref 不可浅拷贝）。
/// `indyBase`：被调方法 indySites 合并进调用方后的起始索引；克隆体里的
/// Intrinsic("indy:<i>") 重写为 indyBase + i（否则指向已析构的 callee 站点）。
std::unique_ptr<SSAInstruction> cloneInstruction(const SSAInstruction& src, const ValueMap& remap,
                                                 ca::str::Utf8StringArena& arena, SSAIds& ids,
                                                 ValueMap& outRemap, ca::usize indyBase)
{
    const auto map = [&](SSAValue* v) { return mapValue(v, remap); };
    const auto str = [&](ca::str::Utf8StringRef r) {
        return r.is_empty() ? r : arena.intern(r);
    };
    const auto indyName = [&](const std::string& name) {
        if (name.rfind("indy:", 0) != 0) return name;
        return "indy:" + std::to_string(indyBase + std::stoull(name.substr(5)));
    };

    std::unique_ptr<SSAInstruction> out;
    switch (src.kind) {
    case SSAInstructionKind::Unary: {
        const auto& s = static_cast<const SSAUnaryInstruction&>(src);
        auto        d = std::make_unique<SSAUnaryInstruction>();
        d->op         = s.op;
        d->value      = map(s.value);
        out           = std::move(d);
        break;
    }
    case SSAInstructionKind::Binary: {
        const auto& s = static_cast<const SSABinaryInstruction&>(src);
        auto        d = std::make_unique<SSABinaryInstruction>();
        d->op         = s.op;
        d->lhs        = map(s.lhs);
        d->rhs        = map(s.rhs);
        out           = std::move(d);
        break;
    }
    case SSAInstructionKind::Compare: {
        const auto& s = static_cast<const SSACompareInstruction&>(src);
        auto        d = std::make_unique<SSACompareInstruction>();
        d->predicate  = s.predicate;
        d->lhs        = map(s.lhs);
        d->rhs        = map(s.rhs);
        out           = std::move(d);
        break;
    }
    case SSAInstructionKind::Convert: {
        const auto& s = static_cast<const SSAConvertInstruction&>(src);
        auto        d = std::make_unique<SSAConvertInstruction>();
        d->convertKind = s.convertKind;
        d->value       = map(s.value);
        d->targetType  = s.targetType;
        d->referenceTypeDesc = str(s.referenceTypeDesc);
        out            = std::move(d);
        break;
    }
    case SSAInstructionKind::LoadField: {
        const auto& s = static_cast<const SSALoadFieldInstruction&>(src);
        auto        d = std::make_unique<SSALoadFieldInstruction>();
        d->field       = {str(s.field.owner), str(s.field.name), str(s.field.desc),
                          s.field.isStatic};
        d->receiver    = map(s.receiver);
        out            = std::move(d);
        break;
    }
    case SSAInstructionKind::StoreField: {
        const auto& s = static_cast<const SSAStoreFieldInstruction&>(src);
        auto        d = std::make_unique<SSAStoreFieldInstruction>();
        d->field       = {str(s.field.owner), str(s.field.name), str(s.field.desc),
                          s.field.isStatic};
        d->receiver    = map(s.receiver);
        d->value       = map(s.value);
        out            = std::move(d);
        break;
    }
    case SSAInstructionKind::ArrayLoad: {
        const auto& s = static_cast<const SSAArrayLoadInstruction&>(src);
        auto        d = std::make_unique<SSAArrayLoadInstruction>();
        d->array       = map(s.array);
        d->index       = map(s.index);
        d->elementType = s.elementType;
        out            = std::move(d);
        break;
    }
    case SSAInstructionKind::ArrayStore: {
        const auto& s = static_cast<const SSAArrayStoreInstruction&>(src);
        auto        d = std::make_unique<SSAArrayStoreInstruction>();
        d->array       = map(s.array);
        d->index       = map(s.index);
        d->value       = map(s.value);
        d->elementType = s.elementType;
        out            = std::move(d);
        break;
    }
    case SSAInstructionKind::Call: {
        const auto& s = static_cast<const SSACallInstruction&>(src);
        auto        d = std::make_unique<SSACallInstruction>();
        d->method      = {str(s.method.owner), str(s.method.name), str(s.method.desc),
                          s.method.invokeOpcode};
        d->args.reserve(s.args.size());
        for (auto* a : s.args) d->args.push_back(map(a));
        out = std::move(d);
        break;
    }
    case SSAInstructionKind::Allocate: {
        const auto& s = static_cast<const SSAAllocateInstruction&>(src);
        auto        d = std::make_unique<SSAAllocateInstruction>();
        d->allocationKind = s.allocationKind;
        d->typeDesc       = str(s.typeDesc);
        d->args.reserve(s.args.size());
        for (auto* a : s.args) d->args.push_back(map(a));
        out = std::move(d);
        break;
    }
    case SSAInstructionKind::Intrinsic: {
        const auto& s = static_cast<const SSAIntrinsicInstruction&>(src);
        auto        d = std::make_unique<SSAIntrinsicInstruction>();
        d->name        = indyName(s.name);
        d->args.reserve(s.args.size());
        for (auto* a : s.args) d->args.push_back(map(a));
        out = std::move(d);
        break;
    }
    case SSAInstructionKind::Barrier: {
        const auto& s = static_cast<const SSABarrierInstruction&>(src);
        auto        d = std::make_unique<SSABarrierInstruction>();
        d->reason       = s.reason;
        out             = std::move(d);
        break;
    }
    }

    if (out == nullptr) return nullptr;
    out->effect = src.effect;
    if (src.hasResult) {
        out->hasResult = true;
        out->result    = SSAInstructionResult(ids.newValueId(), src.result.type);
        outRemap.emplace(&src.result, &out->result);
    }
    return out;
}

/// 克隆被调方法全部块（含合成 entry）到临时容器；返回 mergeBlock 非空时
/// ReturnTerminator 改跳 merge（返回值作 block arg）。
void cloneCalleeBlocks(const SSAFunction& callee, const ValueMap& paramRemap,
                       ca::str::Utf8StringArena& arena, SSAIds& ids, SSABlock* mergeBlock,
                       ValueMap& remap, ca::usize indyBase,
                       std::vector<std::unique_ptr<SSABlock>>& outBlocks)
{
    // 第一遍：建块 + 新 block args。
    std::unordered_map<const SSABlock*, SSABlock*> blockMap;
    for (const auto& src : callee.blocks) {
        auto dst = std::make_unique<SSABlock>(SSABlockId{ids.newBlockId()});
        dst->args.reserve(src->args.size());
        for (const auto& a : src->args) {
            dst->args.push_back(SSABlockArg{ids.newValueId(), a.type, a.slot});
            remap.emplace(&a, &dst->args.back());
        }
        blockMap.emplace(src.get(), dst.get());
        outBlocks.push_back(std::move(dst));
    }

    // 第二遍：指令 + terminator（successor 映射；return → jump mergeBlock）。
    for (const auto& src : callee.blocks) {
        auto* dst = blockMap.at(src.get());
        for (const auto& ins : src->instructions) {
            auto cloned = cloneInstruction(*ins, remap, arena, ids, remap, indyBase);
            if (cloned != nullptr) dst->instructions.push_back(std::move(cloned));
        }
        const auto mapSucc = [&](const SSASuccessor& succ) {
            SSASuccessor out;
            out.block = succ.block == nullptr ? nullptr : blockMap.at(succ.block);
            out.args.reserve(succ.args.size());
            for (auto* a : succ.args) out.args.push_back(mapValue(a, remap));
            return out;
        };
        std::visit(
            [&](auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, SSAJumpTerminator>) {
                    SSAJumpTerminator j;
                    j.target    = mapSucc(t.target);
                    dst->terminator = std::move(j);
                }
                else if constexpr (std::is_same_v<T, SSABranchTerminator>) {
                    SSABranchTerminator b;
                    b.condition   = mapValue(t.condition, remap);
                    b.trueTarget  = mapSucc(t.trueTarget);
                    b.falseTarget = mapSucc(t.falseTarget);
                    dst->terminator = std::move(b);
                }
                else if constexpr (std::is_same_v<T, SSASwitchTerminator>) {
                    SSASwitchTerminator s;
                    s.value = mapValue(t.value, remap);
                    for (const auto& c : t.cases)
                        s.cases.push_back(SSASwitchCase{c.key, mapSucc(c.target)});
                    s.defaultTarget = mapSucc(t.defaultTarget);
                    dst->terminator = std::move(s);
                }
                else if constexpr (std::is_same_v<T, SSAReturnTerminator>) {
                    if (mergeBlock == nullptr) {
                        dst->terminator = SSAUnreachableTerminator{};
                    }
                    else {
                        SSAJumpTerminator j;
                        j.target.block = mergeBlock;
                        if (t.value != nullptr) j.target.args.push_back(mapValue(t.value, remap));
                        dst->terminator = std::move(j);
                    }
                }
                else if constexpr (std::is_same_v<T, SSAThrowTerminator>) {
                    SSAThrowTerminator th;
                    th.value    = mapValue(t.value, remap);
                    dst->terminator = std::move(th);
                }
                else {
                    dst->terminator = SSAUnreachableTerminator{};
                }
            },
            src->terminator);
    }
}

/// 把旧 result 的所有引用（指令 operand / terminator / 异常边）重定向到新值。
void redirectResult(SSAFunction& fn, const SSAValue* oldResult, SSAValue* newValue)
{
    if (oldResult == nullptr) return;
    const auto redirect = [&](SSAValue*& v) {
        if (v == oldResult) v = newValue;
    };
    for (auto& block : fn.blocks) {
        for (auto& ins : block->instructions) {
            switch (ins->kind) {
            case SSAInstructionKind::Unary: {
                auto& s = static_cast<SSAUnaryInstruction&>(*ins);
                redirect(s.value);
                break;
            }
            case SSAInstructionKind::Binary: {
                auto& s = static_cast<SSABinaryInstruction&>(*ins);
                redirect(s.lhs);
                redirect(s.rhs);
                break;
            }
            case SSAInstructionKind::Compare: {
                auto& s = static_cast<SSACompareInstruction&>(*ins);
                redirect(s.lhs);
                redirect(s.rhs);
                break;
            }
            case SSAInstructionKind::Convert: {
                auto& s = static_cast<SSAConvertInstruction&>(*ins);
                redirect(s.value);
                break;
            }
            case SSAInstructionKind::LoadField: {
                auto& s = static_cast<SSALoadFieldInstruction&>(*ins);
                redirect(s.receiver);
                break;
            }
            case SSAInstructionKind::StoreField: {
                auto& s = static_cast<SSAStoreFieldInstruction&>(*ins);
                redirect(s.receiver);
                redirect(s.value);
                break;
            }
            case SSAInstructionKind::ArrayLoad: {
                auto& s = static_cast<SSAArrayLoadInstruction&>(*ins);
                redirect(s.array);
                redirect(s.index);
                break;
            }
            case SSAInstructionKind::ArrayStore: {
                auto& s = static_cast<SSAArrayStoreInstruction&>(*ins);
                redirect(s.array);
                redirect(s.index);
                redirect(s.value);
                break;
            }
            case SSAInstructionKind::Call: {
                auto& s = static_cast<SSACallInstruction&>(*ins);
                for (auto*& a : s.args) redirect(a);
                break;
            }
            case SSAInstructionKind::Allocate: {
                auto& s = static_cast<SSAAllocateInstruction&>(*ins);
                for (auto*& a : s.args) redirect(a);
                break;
            }
            case SSAInstructionKind::Intrinsic: {
                auto& s = static_cast<SSAIntrinsicInstruction&>(*ins);
                for (auto*& a : s.args) redirect(a);
                break;
            }
            case SSAInstructionKind::Barrier: break;
            }
        }
        std::visit(
            [&](auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, SSAJumpTerminator>) {
                    for (auto*& a : t.target.args) redirect(a);
                }
                else if constexpr (std::is_same_v<T, SSABranchTerminator>) {
                    redirect(t.condition);
                    for (auto*& a : t.trueTarget.args) redirect(a);
                    for (auto*& a : t.falseTarget.args) redirect(a);
                }
                else if constexpr (std::is_same_v<T, SSASwitchTerminator>) {
                    redirect(t.value);
                    for (auto& c : t.cases)
                        for (auto*& a : c.target.args) redirect(a);
                    for (auto*& a : t.defaultTarget.args) redirect(a);
                }
                else if constexpr (std::is_same_v<T, SSAReturnTerminator>) {
                    redirect(t.value);
                }
                else if constexpr (std::is_same_v<T, SSAThrowTerminator>) {
                    redirect(t.value);
                }
                else if constexpr (std::is_same_v<T, SSAUnreachableTerminator>) {}
            },
            block->terminator);
    }
    for (auto& region : fn.exceptionRegions) {
        for (auto& edge : region.edges) {
            for (auto*& a : edge.handlerArgs) redirect(a);
        }
    }
}

}   // namespace

SsaInlineStats applySsaInlining(SSAFunction& function, const SsaInlineOptions& options,
                                const SsaInlineResolver& resolver, ca::str::Utf8StringArena& arena)
{
    SsaInlineStats stats;
    if (!options.enable || resolver == nullptr) return stats;

    const std::string callerKey =
        function.symbol.owner + "." + function.symbol.name + function.symbol.desc;

    // 迭代内联：克隆进来的调用点下一轮继续（深度限制）。
    for (ca::u32 depth = 0; depth < options.maxInlineDepth; ++depth) {
        struct Site
        {
            SSABlock*             block = nullptr;
            ca::usize             index = 0;   // 指令在 block->instructions 的下标
            SSACallInstruction*   call  = nullptr;
            std::unique_ptr<SSAFunction> callee;
        };
        std::vector<Site> sites;
        for (auto& block : function.blocks) {
            for (ca::usize i = 0; i < block->instructions.size(); ++i) {
                auto* ins = block->instructions[i].get();
                if (ins->kind != SSAInstructionKind::Call) continue;
                auto* call = static_cast<SSACallInstruction*>(ins);
                // 安全条件 0：只内联静态调用（实例方法的 receiver/this 绑定本批不做）。
                if (call->method.invokeOpcode != Opcodes::INVOKESTATIC) {
                    ++stats.skippedNotInlinable;
                    continue;
                }
                // 安全条件 0.5：调用点排除谓词（RegC：已验证 javac 合成 values()
                // 留给站点 intrinsic，展开体反而多一次 JNI 边界）。
                if (options.excludeMethod && options.excludeMethod(call->method)) {
                    ++stats.skippedExcluded;
                    continue;
                }
                // 安全条件 1：目标可解析。
                auto callee = resolver(call->method);
                if (callee == nullptr) {
                    ++stats.skippedUnresolvable;
                    // #1193 可见化：unresolvable 调用点过去完全静默（内联不发生
                    // 且无任何日志），这里按首次出现去重收集样例，由调用方
                    // （RegCBackend）汇总输出。
                    const std::string sample = call->method.owner.to_std_string() + "." +
                                               call->method.name.to_std_string() + ":" +
                                               call->method.desc.to_std_string();
                    if (stats.unresolvableSamples.size() < size_t(kUnresolvableSampleCap) &&
                        std::find(stats.unresolvableSamples.begin(),
                                  stats.unresolvableSamples.end(),
                                  sample) == stats.unresolvableSamples.end()) {
                        stats.unresolvableSamples.push_back(sample);
                    }
                    continue;
                }
                // 安全条件 2：被调方法无异常区。
                if (!callee->exceptionRegions.empty()) {
                    ++stats.skippedByException;
                    continue;
                }
                // 安全条件 3：大小门控。
                if (countInstructions(*callee) > options.maxCalleeInstructions) {
                    ++stats.skippedBySize;
                    continue;
                }
                // 安全条件 4：非递归（调用链上不含目标自身）。
                const std::string calleeKey = callee->symbol.owner + "." + callee->symbol.name +
                                              callee->symbol.desc;
                if (calleeKey == callerKey) {
                    ++stats.skippedByRecursion;
                    continue;
                }
                // 安全条件 5：被调方法非空块集。克隆体 = 逐源块拷贝
                // （cloneCalleeBlocks 第一遍每源块恰产一块），空块集无 entry
                // 可跳、无体可内联。判定必须在此处（任何迁移发生前）完成：
                // 既往实现把 clones.empty() 判空放在 terminator/tail 迁移之后
                // （#1194），该路径一旦触发，调用块已被 resize 截断（call 指令
                // 销毁）、tail 与 merge 块析构、call result 的既有引用重定向到
                // 已析构的 merge arg——调用块悬空。resolver 契约下不应出现，
                // 但按本 pass 的 fail-closed 契约跳过调用点（call 原样保留），
                // 不用断言：断言在 NDEBUG 下剥除，悬空路径依旧存在。
                if (callee->blocks.empty()) {
                    ++stats.skippedNotInlinable;
                    continue;
                }
                Site site;
                site.block  = block.get();
                site.index  = i;
                site.call   = call;
                site.callee = std::move(callee);
                sites.push_back(std::move(site));
            }
        }
        if (sites.empty()) break;

        // 收集顺序即 function.blocks 顺序（块内指令序）：跨块处理顺序与
        // 堆地址无关，保证块追加顺序确定（不可按裸指针排序——地址随运行变化，
        // 会使克隆体块序/cache 编号非确定，破坏可复现构建）。
        // 同块多个调用点：每块每轮只处理 index 最小的一个；先内联的调用点
        // 克隆体先入 blocks（与执行顺序一致，RegLowering 要求 def-before-use），
        // 后续调用点随 tail 迁移进 merge，下一轮迭代再处理。

        // 预留容量（防 push_back realloc 悬垂裸指针）。
        ca::usize extra = 0;
        for (const auto& site : sites) extra += site.callee->blocks.size() + 1;
        function.blocks.reserve(function.blocks.size() + extra);

        SSABlock* lastBlock = nullptr;
        for (auto& site : sites) {
            // 同一块只处理 index 最小的调用点：每个调用点都要「接管」调用块的
            // terminator 与 tail（迁移到自己的 merge），同块多点批量处理会互相覆盖
            // 产生悬空 CFG；其余调用点随 tail 迁入 merge，下一轮迭代重新收集再内联。
            if (site.block == lastBlock) continue;
            lastBlock = site.block;
            auto& callee = *site.callee;

            // 字面量保活：SSAIntLiteral/SSAFloatLiteral/SSAOpaqueValue 由 SSAFunction
            // 拥有，克隆体直接引用原指针；callee 即将析构，必须先把这些值搬进 caller
            // （unique_ptr 转移不改变堆对象地址，克隆体引用依然有效）。null 字面量特判：
            // caller 已有 null 时重定向克隆体引用。
            ValueMap remap;
            for (auto& lit : callee.intLiterals) function.intLiterals.push_back(std::move(lit));
            for (auto& lit : callee.floatLiterals) function.floatLiterals.push_back(std::move(lit));
            for (auto& v : callee.opaqueValues) function.opaqueValues.push_back(std::move(v));
            if (function.nullLiteral == nullptr) {
                function.nullLiteral = std::move(callee.nullLiteral);
            }
            else if (callee.nullLiteral != nullptr) {
                remap.emplace(callee.nullLiteral.get(), function.nullLiteral.get());
            }

            // 参数绑定：callee.parameters[i] -> 调用点 args[i]（静态调用一一对应）。
            for (ca::usize i = 0; i < callee.parameters.size() && i < site.call->args.size(); ++i) {
                remap.emplace(callee.parameters[i].get(), site.call->args[i]);
            }

            // indySites 合并：克隆体里的 Intrinsic("indy:<i>") 引用被调方法的站点，
            // 先把站点搬进 caller（value 语义，move 后索引重映射由 clone 侧完成）。
            const ca::usize indyBase = function.indySites.size();
            function.indySites.insert(function.indySites.end(),
                                      std::make_move_iterator(callee.indySites.begin()),
                                      std::make_move_iterator(callee.indySites.end()));

            // 返回汇合块：非 void 时 1 个 block arg；宿主 = 调用点之后的指令。
            auto mergeBlock = std::make_unique<SSABlock>(SSABlockId{function.ids.newBlockId()});
            SSABlock* mergePtr = mergeBlock.get();
            const bool hasReturn = callee.symbol.returnType != SSAType::UNKNOWN &&
                                   callee.symbol.returnType != SSAType::VOID;
            if (hasReturn) {
                mergeBlock->args.push_back(
                    SSABlockArg{function.ids.newValueId(), callee.symbol.returnType, 0});
            }

            // redirect 必须在 resize 之前、克隆之前完成：oldResult 是「地址」，
            // call 指令释放后 malloc 可能把同一地址分给克隆体，地址比较会误伤。
            // 此时调用块 terminator / tail 的 oldResult 引用被改到 merge arg。
            const SSAValue* oldResult = nullptr;
            if (site.call->hasResult) oldResult = &site.call->result;
            SSAValue* resultValue = nullptr;
            if (hasReturn) resultValue = &mergePtr->args.front();
            if (oldResult != nullptr) redirectResult(function, oldResult, resultValue);
            // merge 出口 = 调用块原 terminator（redirect 已完成，拷贝即最终形态）。
            mergeBlock->terminator = site.block->terminator;

            // 调用点之后的指令在值依赖上位于内联体之后（如 `x = callee(...); store f, x`），
            // 必须迁移到 merge 块（否则留在调用块会在克隆体计算前执行，读到未定义值）。
            auto& insns = site.block->instructions;
            std::vector<std::unique_ptr<SSAInstruction>> tail;
            for (ca::usize i = site.index + 1; i < insns.size(); ++i) {
                tail.push_back(std::move(insns[i]));
            }
            insns.resize(site.index);
            mergeBlock->instructions = std::move(tail);

            // 克隆被调方法全部块。clones 非空由安全条件 5 结构性保证
            // （每源块恰产一块，callee.blocks 非空）——#1194 之前这里的
            // clones.empty() 迟到判空发生在迁移之后，已删除。
            std::vector<std::unique_ptr<SSABlock>> clones;
            cloneCalleeBlocks(callee, remap, arena, function.ids, mergePtr, remap, indyBase,
                              clones);

            // #1188：调用块位于异常受保护区时，克隆体与 merge 块同样落在保护
            // 范围内（被内联体的 mayThrow 调用点在 try 语义下必须分发到
            // handler），必须登记为受保护块并补异常边——否则发射端
            // activeExceptionRegions 对新块为空，逐调用 pending 异常检查被
            // 省略（emitInstructionWithCheck 仅对受保护块发射），挂起异常
            // 越过 try 边界继续执行（try{Enum.valueOf}catch(IAE) 吞异常）。
            // handlerArgs 沿用调用块现有边的实参（调用点之前的帧状态）：被
            // 内联体只定义新值、不改写 handler 可见的局部槽，尾部指令仅在内
            // 联体成功完成后执行——对克隆体内任意抛出点都是正确传输；merge
            // 块沿用同一组实参与既有「块入口帧状态」精度约定一致（#1188 评审
            // P3-3 勘注：merge 内尾部指令自身的内部抛出点传输精度=块入口帧
            // 状态，非精确帧状态——与修复前该指令留在调用块内时相同，无回归）。
            if (!function.exceptionRegions.empty()) {
                std::vector<SSABlock*> newBlocks;
                newBlocks.reserve(clones.size() + 1);
                for (const auto& c : clones) newBlocks.push_back(c.get());
                newBlocks.push_back(mergePtr);
                for (auto& region : function.exceptionRegions) {
                    const SSAExceptionEdge* hostEdge = nullptr;
                    for (const auto& edge : region.edges) {
                        if (edge.protectedBlock == site.block) {
                            hostEdge = &edge;
                            break;
                        }
                    }
                    if (hostEdge == nullptr) continue;   // 防御：调用块无边则保持原状
                    // 先拷贝传输实参：edges.push_back 可能扩容令 hostEdge 悬垂。
                    std::vector<SSAValue*> transferred = hostEdge->handlerArgs;
                    for (SSABlock* newBlock : newBlocks) {
                        region.protectedBlocks.push_back(newBlock);
                        SSAExceptionEdge protectedEdge;
                        protectedEdge.protectedBlock = newBlock;
                        protectedEdge.handlerArgs    = transferred;
                        region.edges.push_back(std::move(protectedEdge));
                    }
                }
            }

            // 接线：调用块 -> 克隆 entry；克隆体在前、merge 在后
            // （merge 的 terminator/tail 使用克隆体定义的 merge arg，块序必须
            //  满足 def-before-use，RegLowering 按块序检查）。
            site.block->terminator = SSAJumpTerminator{SSASuccessor{clones.front().get(), {}}};
            for (auto& c : clones) function.blocks.push_back(std::move(c));
            function.blocks.push_back(std::move(mergeBlock));
            ++stats.inlinedCalls;
        }
    }
    return stats;
}

}   // namespace mj2x

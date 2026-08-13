/// @file regc_intrinsics.cpp
/// @brief RegC 发射器 native 对象分析与 intrinsic 调用发射（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// StringBuilder/StringBuffer 分析（analyzeStringBuilders/analyzeNativeWrappers）、
/// native 包装对象逃逸检查、materialize 发射与 intrinsic 调用发射。
/// 从原 regc_emitter.cpp 纯搬移，生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/format.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {

using namespace jbytecode::core;

using codegen::cStringLiteral;
using codegen::CSourceStream;

void emitRegCStatusCheck(CSourceStream& out, RegCFunctionEmitState& state,
                         const ca::str::Utf8StringRef& statusExpr,
                         const std::string&            operation)
{
    const int temp = state.stringBuilderTempIndex++;
    out.emitLine(ca::str::format_std("mj2x_regc_status runtime_status_{} = {};", temp, statusExpr));
    out.iff(ca::str::format_std(
                "runtime_status_{} != MJ2X_REGC_STATUS_OK && runtime_status_{} != "
                "MJ2X_REGC_STATUS_PENDING_EXCEPTION && !_mj2x_regc_exception_check()",
                temp, temp),
        [&](CSourceStream& u) {
            u.emitLine(ca::str::format_std(
                "mj2x_regc_report_status(_mj2x_regc_current_context(), runtime_status_{}, {});",
                temp, cStringLiteral(operation)));
        });
}

ca::str::Utf8String nativeObjectStorageName(RegId id)
{
    return ca::str::format("native_r{}", id.value);
}

bool emitMaterializedStringBuilder(CSourceStream& out, RegCFunctionEmitState& state,
                                   const RegOperand& operand, ca::str::Utf8String& expression)
{
    const RegCNativeObjectState* objectState = nativeObjectFor(state, operand);
    if (objectState == nullptr || !isMutableStringKind(objectState->kind)) {
        expression = operandExpr(operand);
        return false;
    }
    if (operand.kind != RegOperandKind::Register) {
        expression = operandExpr(operand);
        return false;
    }
    const auto ownerIt = state.nativeStringBuilderOwnerByRegister.find(operand.reg.value);
    if (ownerIt == state.nativeStringBuilderOwnerByRegister.end()) {
        expression = operandExpr(operand);
        return false;
    }

    const RegId owner{ownerIt->second};
    expression = stringBuilderMaterializedName(owner);
    const char* materializeSymbol =
        objectState->kind == RegCIntrinsicNativeObjectKind::StringBuffer
            ? "mj2x_string_buffer_materialize"
            : "mj2x_string_builder_materialize";
    const std::string operation =
        std::string(mutableStringDisplayName(objectState->kind)) + ".materialize";
    out.iff(expression.to_std_string() + " == NULL && !_mj2x_regc_exception_check()",
            [&](CSourceStream& u) {
                const std::string statusExpr = codegen::exprCall(
                    materializeSymbol,
                    {"_mj2x_regc_current_context()",
                     stringBuilderStateExpr(operand, objectState->kind).to_std_string(),
                     "&" + expression.to_std_string()});
                emitRegCStatusCheck(u, state,
                                    ca::str::Utf8StringRef::from_string_view(statusExpr),
                                    operation);
            });
    return true;
}

bool rejectNativeObjectEscape(const RegCFunctionEmitState& state, const RegOperand& operand,
                              const std::string& surface, std::string& reason)
{
    if (!nativeObjectFor(state, operand)) return true;
    reason = "native object escaped through " + surface;
    return false;
}

bool rejectNativeObjectEscapes(const RegCFunctionEmitState&   state,
                               const std::vector<RegOperand>& operands, const std::string& surface,
                               std::string& reason)
{
    for (const auto& operand : operands) {
        if (!rejectNativeObjectEscape(state, operand, surface, reason)) return false;
    }
    return true;
}

namespace {

class RegIdUnionFind
{
public:
    void add(uint32_t id) { parent_.emplace(id, id); }

    uint32_t find(uint32_t id)
    {
        add(id);
        uint32_t parent = parent_[id];
        if (parent == id) return id;
        parent_[id] = find(parent);
        return parent_[id];
    }

    void unite(uint32_t lhs, uint32_t rhs)
    {
        const uint32_t lhsRoot = find(lhs);
        const uint32_t rhsRoot = find(rhs);
        if (lhsRoot != rhsRoot) parent_[rhsRoot] = lhsRoot;
    }

    const std::map<uint32_t, uint32_t>& entries() const { return parent_; }

private:
    std::map<uint32_t, uint32_t> parent_;
};

bool referenceRegister(const RegOperand& operand, uint32_t& id)
{
    if (operand.kind != RegOperandKind::Register || operand.type != SSAType::REF) return false;
    id = operand.reg.value;
    return true;
}

void connectReferenceCopy(RegIdUnionFind& ids, RegValue dest, const RegOperand& source)
{
    uint32_t sourceId = 0;
    if (dest.type == SSAType::REF && referenceRegister(source, sourceId)) {
        ids.unite(dest.id.value, sourceId);
    }
}

void connectTerminatorCopies(RegIdUnionFind& ids, const RegTerminator& terminator)
{
    std::visit(
        [&](const auto& t) {
            using T      = std::decay_t<decltype(t)>;
            auto connect = [&](const std::vector<RegCopy>& copies) {
                for (const auto& copy : copies) connectReferenceCopy(ids, copy.dest, copy.source);
            };
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                connect(t.target.copies);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                connect(t.trueTarget.copies);
                connect(t.falseTarget.copies);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                for (const auto& item : t.cases) connect(item.target.copies);
                connect(t.defaultTarget.copies);
            }
        },
        terminator);
}

}   // namespace

RegCStringBuilderPlan analyzeStringBuilders(const RegFunction& fn)
{
    RegIdUnionFind                                                  ids;
    std::vector<std::pair<uint32_t, RegCIntrinsicNativeObjectKind>> allocations;

    for (const auto& parameter : fn.parameters) {
        if (parameter.type == SSAType::REF) ids.add(parameter.id.value);
    }
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && ins.dest.type == SSAType::REF) ids.add(ins.dest.id.value);
            if (ins.kind == RegInstructionKind::Copy) {
                connectReferenceCopy(ids, ins.copy.dest, ins.copy.source);
            }
            else if (ins.kind == RegInstructionKind::Allocate &&
                     ins.allocationKind == SSAAllocationKind::Object &&
                     (ins.typeDesc == "java/lang/StringBuilder" ||
                      ins.typeDesc == "java/lang/StringBuffer")) {
                const auto kind = ins.typeDesc == "java/lang/StringBuffer"
                                      ? RegCIntrinsicNativeObjectKind::StringBuffer
                                      : RegCIntrinsicNativeObjectKind::StringBuilder;
                allocations.emplace_back(ins.dest.id.value, kind);
                ids.add(ins.dest.id.value);
            }
            else if (ins.kind == RegInstructionKind::Call) {
                const RegCIntrinsicDecision decision =
                    RegCIntrinsicRegistry::builtins().matchCall(ins);
                if (decision.kind == RegCIntrinsicDecisionKind::Emit &&
                    decision.emitKind == RegCIntrinsicEmitKind::StringBuilderAppend &&
                    ins.hasDest && !ins.args.empty()) {
                    uint32_t receiverId = 0;
                    if (referenceRegister(ins.args[0], receiverId)) {
                        ids.unite(ins.dest.id.value, receiverId);
                    }
                }
            }
        }
        connectTerminatorCopies(ids, block->terminator);
    }

    std::set<uint32_t>                                builderRoots;
    std::map<uint32_t, RegCIntrinsicNativeObjectKind> kindByRoot;
    for (const auto& [allocation, kind] : allocations) {
        const uint32_t root = ids.find(allocation);
        builderRoots.insert(root);
        kindByRoot.emplace(root, kind);
    }

    auto builderRoot = [&](const RegOperand& operand, uint32_t& root) {
        uint32_t id = 0;
        if (!referenceRegister(operand, id)) return false;
        root = ids.find(id);
        return builderRoots.count(root) != 0;
    };
    std::set<uint32_t> fallbackRoots;
    auto               fallback = [&](const RegOperand& operand) {
        uint32_t root = 0;
        if (builderRoot(operand, root)) fallbackRoots.insert(root);
    };
    auto fallbackAll = [&](const std::vector<RegOperand>& operands, size_t first) {
        for (size_t i = first; i < operands.size(); ++i) fallback(operands[i]);
    };

    struct BuilderEvent
    {
        const RegBlock* block;
        size_t          index;
    };
    std::map<uint32_t, std::vector<BuilderEvent>> escapeEvents;
    std::map<uint32_t, std::vector<BuilderEvent>> nativeUseEvents;
    auto recordEvent = [&](std::map<uint32_t, std::vector<BuilderEvent>>& events,
                           const RegOperand&                              operand,
                           const RegBlock*                                block,
                           size_t                                         index) {
        uint32_t root = 0;
        if (builderRoot(operand, root)) events[root].push_back({block, index});
    };

    for (const auto& block : fn.blocks) {
        for (size_t instructionIndex = 0; instructionIndex < block->instructions.size();
             ++instructionIndex) {
            const auto& ins = block->instructions[instructionIndex];
            switch (ins.kind) {
            case RegInstructionKind::Copy:
            case RegInstructionKind::Allocate:
            case RegInstructionKind::Barrier: break;
            case RegInstructionKind::Unary: fallback(ins.value); break;
            case RegInstructionKind::Binary:
            case RegInstructionKind::Compare:
                fallback(ins.lhs);
                fallback(ins.rhs);
                break;
            case RegInstructionKind::Convert: fallback(ins.value); break;
            case RegInstructionKind::LoadField: fallback(ins.receiver); break;
            case RegInstructionKind::StoreField:
                fallback(ins.receiver);
                recordEvent(escapeEvents, ins.value, block.get(), instructionIndex);
                break;
            case RegInstructionKind::ArrayLoad:
                fallback(ins.receiver);
                fallback(ins.index);
                break;
            case RegInstructionKind::ArrayStore:
                fallback(ins.receiver);
                fallback(ins.index);
                recordEvent(escapeEvents, ins.value, block.get(), instructionIndex);
                break;
            case RegInstructionKind::Intrinsic: fallbackAll(ins.args, 0); break;
            case RegInstructionKind::Call:
            {
                const RegCIntrinsicDecision decision =
                    RegCIntrinsicRegistry::builtins().matchCall(ins);
                const bool safeInit = decision.kind == RegCIntrinsicDecisionKind::Emit &&
                                      decision.emitKind == RegCIntrinsicEmitKind::StringBuilderInit;
                const bool safeAppend =
                    decision.kind == RegCIntrinsicDecisionKind::Emit &&
                    decision.emitKind == RegCIntrinsicEmitKind::StringBuilderAppend;
                const bool safeMethod =
                    decision.kind == RegCIntrinsicDecisionKind::Emit &&
                    ((decision.emitKind == RegCIntrinsicEmitKind::NativeObjectMethod &&
                      isMutableStringKind(decision.nativeObjectKind)) ||
                     decision.emitKind == RegCIntrinsicEmitKind::StringBuilderMethod);
                if (safeInit || safeAppend || safeMethod) {
                    if (!ins.args.empty()) {
                        recordEvent(nativeUseEvents, ins.args[0], block.get(), instructionIndex);
                    }
                    fallbackAll(ins.args, 1);
                }
                else {
                    for (const auto& arg : ins.args) {
                        recordEvent(escapeEvents, arg, block.get(), instructionIndex);
                    }
                }
                break;
            }
            }
        }

        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    fallback(t.condition);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    fallback(t.value);
                }
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    fallback(t.value);
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (t.hasValue) {
                        recordEvent(escapeEvents, t.value, block.get(), block->instructions.size());
                    }
                }
            },
            block->terminator);
    }

    std::map<uint32_t, size_t>   allocationCounts;
    std::map<uint32_t, uint32_t> ownerByRoot;
    for (const auto& [allocation, unusedKind] : allocations) {
        (void)unusedKind;
        const uint32_t root = ids.find(allocation);
        ++allocationCounts[root];
        ownerByRoot.emplace(root, allocation);
    }
    for (const auto& [root, count] : allocationCounts) {
        if (count > 1) fallbackRoots.insert(root);
    }

    auto successors = [](const RegBlock* block) {
        std::vector<const RegBlock*> result;
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    result.push_back(t.target.block);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    result.push_back(t.trueTarget.block);
                    result.push_back(t.falseTarget.block);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& item : t.cases) result.push_back(item.target.block);
                    result.push_back(t.defaultTarget.block);
                }
            },
            block->terminator);
        return result;
    };

    for (const auto& [root, escapes] : escapeEvents) {
        const auto useIt = nativeUseEvents.find(root);
        if (useIt == nativeUseEvents.end()) continue;
        const auto& uses = useIt->second;
        for (const auto& escape : escapes) {
            bool useAfterEscape = false;
            for (const auto& use : uses) {
                if (use.block == escape.block && use.index > escape.index) {
                    useAfterEscape = true;
                    break;
                }
            }
            if (useAfterEscape) {
                fallbackRoots.insert(root);
                break;
            }

            std::vector<const RegBlock*> worklist = successors(escape.block);
            std::set<const RegBlock*>    visited;
            while (!worklist.empty() && !useAfterEscape) {
                const RegBlock* current = worklist.back();
                worklist.pop_back();
                if (current == nullptr || !visited.insert(current).second) continue;
                for (const auto& use : uses) {
                    if (use.block == current) {
                        useAfterEscape = true;
                        break;
                    }
                }
                if (!useAfterEscape) {
                    auto next = successors(current);
                    worklist.insert(worklist.end(), next.begin(), next.end());
                }
            }
            if (useAfterEscape) {
                fallbackRoots.insert(root);
                break;
            }
        }
    }

    RegCStringBuilderPlan plan;
    for (const auto& [id, unused] : ids.entries()) {
        (void)unused;
        const uint32_t root = ids.find(id);
        if (builderRoots.count(root) != 0 && fallbackRoots.count(root) == 0) {
            plan.nativeRegisters.insert(id);
            plan.ownerByRegister.emplace(id, ownerByRoot[root]);
            plan.kindByRegister.emplace(id, kindByRoot[root]);
        }
    }
    for (const auto& [allocation, unusedKind] : allocations) {
        (void)unusedKind;
        if (plan.nativeRegisters.count(allocation) != 0) {
            plan.ownerRegisters.push_back(allocation);
        }
    }
    return plan;
}

std::set<uint32_t> analyzeNativeWrappers(const RegFunction& fn)
{
    RegIdUnionFind ids;
    for (const auto& parameter : fn.parameters) {
        if (parameter.type == SSAType::REF) ids.add(parameter.id.value);
    }
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && ins.dest.type == SSAType::REF) ids.add(ins.dest.id.value);
            if (ins.kind == RegInstructionKind::Copy) {
                connectReferenceCopy(ids, ins.copy.dest, ins.copy.source);
            }
        }
        connectTerminatorCopies(ids, block->terminator);
    }

    std::map<uint32_t, RegCIntrinsicNativeObjectKind> wrapperRoots;
    std::set<uint32_t>                                fallbackRoots;
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.kind != RegInstructionKind::Call || !ins.hasDest ||
                ins.dest.type != SSAType::REF) {
                continue;
            }
            const RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
            if (decision.kind != RegCIntrinsicDecisionKind::Emit ||
                decision.emitKind != RegCIntrinsicEmitKind::WrapperValueOf) {
                continue;
            }
            const uint32_t root = ids.find(ins.dest.id.value);
            auto [it, inserted] = wrapperRoots.emplace(root, decision.nativeObjectKind);
            if (!inserted && it->second != decision.nativeObjectKind) fallbackRoots.insert(root);
        }
    }

    auto wrapperRoot = [&](const RegOperand& operand, uint32_t& root) {
        uint32_t id = 0;
        if (!referenceRegister(operand, id)) return false;
        root = ids.find(id);
        return wrapperRoots.count(root) != 0;
    };
    auto fallback = [&](const RegOperand& operand) {
        uint32_t root = 0;
        if (wrapperRoot(operand, root)) fallbackRoots.insert(root);
    };
    auto fallbackAll = [&](const std::vector<RegOperand>& operands, size_t first) {
        for (size_t i = first; i < operands.size(); ++i) fallback(operands[i]);
    };

    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            switch (ins.kind) {
            case RegInstructionKind::Copy:
            case RegInstructionKind::Allocate:
            case RegInstructionKind::Barrier: break;
            case RegInstructionKind::Unary: fallback(ins.value); break;
            case RegInstructionKind::Binary:
            case RegInstructionKind::Compare:
                fallback(ins.lhs);
                fallback(ins.rhs);
                break;
            case RegInstructionKind::Convert: fallback(ins.value); break;
            case RegInstructionKind::LoadField: fallback(ins.receiver); break;
            case RegInstructionKind::StoreField:
                fallback(ins.receiver);
                fallback(ins.value);
                break;
            case RegInstructionKind::ArrayLoad:
                fallback(ins.receiver);
                fallback(ins.index);
                break;
            case RegInstructionKind::ArrayStore:
                fallback(ins.receiver);
                fallback(ins.index);
                fallback(ins.value);
                break;
            case RegInstructionKind::Intrinsic: fallbackAll(ins.args, 0); break;
            case RegInstructionKind::Call:
            {
                const RegCIntrinsicDecision decision =
                    RegCIntrinsicRegistry::builtins().matchCall(ins);
                if (decision.kind == RegCIntrinsicDecisionKind::Emit &&
                    decision.emitKind == RegCIntrinsicEmitKind::WrapperUnbox && !ins.args.empty()) {
                    uint32_t root = 0;
                    if (wrapperRoot(ins.args[0], root) &&
                        wrapperRoots[root] != decision.nativeObjectKind) {
                        fallbackRoots.insert(root);
                    }
                    fallbackAll(ins.args, 1);
                }
                else {
                    fallbackAll(ins.args, 0);
                }
                break;
            }
            }
        }
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    fallback(t.condition);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    fallback(t.value);
                }
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    fallback(t.value);
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (t.hasValue) fallback(t.value);
                }
            },
            block->terminator);
    }

    std::set<uint32_t> nativeRegisters;
    for (const auto& [id, unused] : ids.entries()) {
        (void)unused;
        const uint32_t root = ids.find(id);
        if (wrapperRoots.count(root) != 0 && fallbackRoots.count(root) == 0) {
            nativeRegisters.insert(id);
        }
    }
    return nativeRegisters;
}

RegCIntrinsicDecision emitIntrinsicCall(CSourceStream& out,
                                        const RegInstruction& ins, RegCFunctionEmitState& state)
{
    RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
    if (decision.kind != RegCIntrinsicDecisionKind::Emit) return decision;

    if (decision.emitKind == RegCIntrinsicEmitKind::WrapperValueOf) {
        if (!ins.hasDest) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "wrapper valueOf intrinsic missing destination";
            return decision;
        }
        if (state.nativeWrapperRegisters.count(ins.dest.id.value) == 0) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "wrapper value escapes native state";
            return decision;
        }
        out.appendLineStart(nativeObjectType(decision.nativeObjectKind))
            .append(" ")
            .append(nativeObjectStorageName(ins.dest.id))
            .append(" = ");
        std::vector<std::string> valueOfArgs;
        if (!ins.args.empty()) valueOfArgs.push_back(operandExpr(ins.args[0]).to_std_string());
        out.append(codegen::exprCallArgs(decision.symbol, valueOfArgs));
        out.append(";").newLine();
        const ca::str::Utf8String storageName = nativeObjectStorageName(ins.dest.id);
        const std::string         destName    = regName(ins.dest.id);
        out.assign(destName, "NULL");
        state.nativeObjects[ins.dest.id.value] =
            RegCNativeObjectState{decision.nativeObjectKind, storageName.to_std_string()};
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::WrapperUnbox) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "wrapper unbox intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != decision.nativeObjectKind) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "wrapper receiver is not native state";
            return decision;
        }
        out.appendLineStart();
        if (ins.hasDest) {
            out.append(regName(ins.dest.id)).append(" = ");
        }
        out.append(codegen::exprCall(decision.symbol, {objectState->storageName}));
        out.append(";").newLine();
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::RandomInit) {
        if (ins.args.size() != 2) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "Random init intrinsic requires receiver and seed";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != RegCIntrinsicNativeObjectKind::Random) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "Random receiver is not native state";
            return decision;
        }
        out.appendLineStart()
            .append(codegen::exprCall(decision.symbol,
                                      {"&" + objectState->storageName,
                                       operandExpr(ins.args[1]).to_std_string()}))
            .append(";")
            .newLine();
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::NativeObjectMethod) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "native object method intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != decision.nativeObjectKind) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "receiver is not native object state";
            return decision;
        }
        out.appendLineStart();
        if (ins.hasDest) {
            out.append(regName(ins.dest.id)).append(" = ");
        }
        std::vector<std::string> nativeArgs;
        nativeArgs.push_back("&" + objectState->storageName);
        for (size_t i = 1; i < ins.args.size(); ++i) {
            nativeArgs.push_back(operandExpr(ins.args[i]).to_std_string());
        }
        out.append(codegen::exprCallArgs(decision.symbol, nativeArgs));
        out.append(";").newLine();
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringBuilderMethod) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "mutable string method intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != decision.nativeObjectKind ||
            !isMutableStringKind(objectState->kind)) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "mutable string receiver is not matching native state";
            return decision;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            if (nativeObjectFor(state, ins.args[i])) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "mutable string method argument is native state";
                return decision;
            }
        }
        std::vector<std::string> callArgs;
        callArgs.push_back("_mj2x_regc_current_context()");
        callArgs.push_back(stringBuilderStateExpr(ins.args[0], objectState->kind).to_std_string());
        for (size_t i = 1; i < ins.args.size(); ++i) {
            callArgs.push_back(operandExpr(ins.args[i]).to_std_string());
        }
        const int valueTemp = state.stringBuilderTempIndex++;
        if (ins.hasDest) {
            const char* resultType = ins.method.name == "charAt" && ins.method.desc == "(I)C"
                                         ? "uint16_t"
                                         : cType(ins.dest.type);
            out.appendLineStart(resultType)
                .append(" mutable_string_value_")
                .append(ca::str::format_std("{}", valueTemp))
                .append(" = ")
                .append(defaultValue(ins.dest.type))
                .append(";")
                .newLine();
            callArgs.push_back(ca::str::format_std("&mutable_string_value_{}", valueTemp));
        }
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out,
                            state,
                            ca::str::Utf8StringRef::from_string_view(callText),
                            std::string(mutableStringDisplayName(objectState->kind)) + ".method");
        if (ins.hasDest) {
            out.assign(regName(ins.dest.id),
                       ca::str::format_std("mutable_string_value_{}", valueTemp));
        }
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringBuilderInit) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "StringBuilder init intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != decision.nativeObjectKind ||
            !isMutableStringKind(objectState->kind)) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "mutable string receiver is not matching native state";
            return decision;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            if (nativeObjectFor(state, ins.args[i])) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "StringBuilder init argument is native state";
                return decision;
            }
        }
        std::vector<std::string> callArgs;
        callArgs.push_back("_mj2x_regc_current_context()");
        if (ins.args.size() == 1) {
            callArgs.push_back(
                stringBuilderHandleExpr(ins.args[0], objectState->kind).to_std_string());
        }
        else {
            callArgs.push_back(operandExpr(ins.args[1]).to_std_string());
            callArgs.push_back(
                stringBuilderHandleExpr(ins.args[0], objectState->kind).to_std_string());
        }
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out,
                            state,
                            ca::str::Utf8StringRef::from_string_view(callText),
                            std::string(mutableStringDisplayName(objectState->kind)) + ".<init>");
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringBuilderAppend) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "StringBuilder append intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != decision.nativeObjectKind ||
            !isMutableStringKind(objectState->kind)) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "mutable string append receiver is not matching native state";
            return decision;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            if (nativeObjectFor(state, ins.args[i])) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "StringBuilder append argument is native state";
                return decision;
            }
        }
        std::vector<std::string> callArgs;
        callArgs.push_back("_mj2x_regc_current_context()");
        callArgs.push_back(stringBuilderStateExpr(ins.args[0], objectState->kind).to_std_string());
        for (size_t i = 1; i < ins.args.size(); ++i) {
            callArgs.push_back(operandExpr(ins.args[i]).to_std_string());
        }
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out,
                            state,
                            ca::str::Utf8StringRef::from_string_view(callText),
                            std::string(mutableStringDisplayName(objectState->kind)) + ".append");
        if (ins.hasDest) {
            out.assign(regName(ins.dest.id),
                       operandExpr(ins.args[0]).to_std_string());
            state.nativeObjects[ins.dest.id.value] =
                RegCNativeObjectState{objectState->kind,
                                      regName(ins.dest.id)};
        }
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringRuntimeMethod) {
        if (!ins.hasDest) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "String runtime intrinsic missing destination";
            return decision;
        }
        for (const auto& arg : ins.args) {
            if (nativeObjectFor(state, arg)) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "String runtime intrinsic argument is native state";
                return decision;
            }
        }
        const int valueTemp = state.stringBuilderTempIndex++;
        out.appendLineStart(cType(ins.dest.type))
            .append(" string_value_")
            .append(ca::str::format_std("{}", valueTemp))
            .append(" = ")
            .append(defaultValue(ins.dest.type))
            .append(";")
            .newLine();
        std::vector<std::string> callArgs;
        callArgs.push_back("_mj2x_regc_current_context()");
        for (const auto& arg : ins.args) {
            callArgs.push_back(operandExpr(arg).to_std_string());
        }
        callArgs.push_back(ca::str::format_std("&string_value_{}", valueTemp));
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out, state, ca::str::Utf8StringRef::from_string_view(callText),
                            "String intrinsic");
        out.assign(regName(ins.dest.id),
                   ca::str::format_std("string_value_{}", valueTemp));
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::ObjectGetClass) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "getClass intrinsic missing receiver";
            return decision;
        }
        // 语义 == Object.getClass()：返回 receiver 的运行时类。发射为 JNI
        // GetObjectClass（1 个 JNI），替代 resolved dispatch 的 GetObjectClass +
        // IsSameObject/IsInstanceOf 检查 + DeleteLocalRef（2-3 个 JNI）。
        // GetObjectClass 不抛 Java 异常；NULL receiver 显式抛 NPE（与 Java
        // 语义一致，emitter 对无异常区域的方法省略调用后 check，见
        // regc_emitter.cpp isObjectGetClass）。receiver 是 SSA 寄存器（纯
        // 标识符），两次引用无重复求值问题。
        const std::string receiverExpr = operandExpr(ins.args[0]).to_std_string();
        const std::string destName     = ins.hasDest ? regName(ins.dest.id) : std::string();
        out.iff(receiverExpr + " == NULL",
                [&](CSourceStream& u) {
                    u.emitLine("_mj2x_regc_throw_null(\"Object.getClass\");");
                    if (ins.hasDest) u.emitLine(destName + " = NULL;");
                },
                [&](CSourceStream& u) {
                    if (ins.hasDest) u.appendLineStart(destName).append(" = ");
                    u.append(codegen::exprCall("(*_mj2x_regc_env)->GetObjectClass",
                                               {"_mj2x_regc_env", receiverExpr}));
                    u.append(";").newLine();
                });
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringMethod) {
        for (const auto& arg : ins.args) {
            if (nativeObjectFor(state, arg)) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "String intrinsic argument is native state";
                return decision;
            }
        }
        const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
        std::vector<std::string> stringArgs;
        stringArgs.reserve(ins.args.size());
        for (const auto& arg : ins.args) {
            stringArgs.push_back(operandExpr(arg).to_std_string());
        }
        out.appendLineStart();
        if (ins.hasDest) {
            out.append(destName).append(" = ");
        }
        out.append(codegen::exprCallArgs(decision.symbol, stringArgs));
        out.append(";").newLine();
        return decision;
    }

    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
    std::vector<std::string> fallbackArgs;
    fallbackArgs.reserve(ins.args.size() + 1);
    if (decision.passContext) fallbackArgs.push_back("_mj2x_regc_current_context()");
    for (const auto& arg : ins.args) {
        fallbackArgs.push_back(operandExpr(arg).to_std_string());
    }
    out.appendLineStart();
    if (ins.hasDest) {
        out.append(destName).append(" = ");
    }
    out.append(codegen::exprCallArgs(decision.symbol, fallbackArgs));
    out.append(";").newLine();
    return decision;
}
}   // namespace mj2x

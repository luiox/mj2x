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

RegCStringBuilderPlan analyzeStringBuilders(const RegFunction& fn,
                                            const RegCIntrinsicRegistry* registry)
{
    const RegCIntrinsicRegistry& intrinsicRegistry =
        registry == nullptr ? RegCIntrinsicRegistry::builtins() : *registry;
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
                    intrinsicRegistry.matchCall(ins);
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

/// 枚举子类 owner 判定：编译输入内的超类链抵达 java/lang/Enum。
///
/// @details
/// 只信任编译输入的 classNode 层级：JDK/第三方枚举不在索引（getClass 返回
/// nullptr）时无法验证，保守返回 false（调用点保持原 resolved/cached 路径）。
/// java/lang/Enum 自身由 registry 精确匹配，不经此判定。
bool isEnumSubtypeOwner(CompilerIndex* classIndex, const ca::str::Utf8StringRef& owner)
{
    if (classIndex == nullptr) return false;
    std::string cur = owner.to_std_string();
    for (int depth = 0; !cur.empty() && depth < 64; ++depth) {
        CompilerClass* cls = classIndex->getClass(cur);
        if (cls == nullptr || cls->classNode == nullptr) return false;
        const ca::str::Utf8StringRef super = cls->classNode->superName;
        if (super == "java/lang/Enum") return true;
        cur = super.to_std_string();
    }
    return false;
}

/// 跳过 LABEL/LINE/FRAME 元节点，返回下一条真实指令（可能为 nullptr）。
const jbytecode::tree::AbstractInsnNode* nextRealInsn(
    const jbytecode::tree::AbstractInsnNode* node)
{
    while (node != nullptr) {
        const int type = node->getType();
        if (type != jbytecode::tree::AbstractInsnNode::LABEL &&
            type != jbytecode::tree::AbstractInsnNode::LINE &&
            type != jbytecode::tree::AbstractInsnNode::FRAME) {
            break;
        }
        node = node->getNext();
    }
    return node;
}

/// 在编译类里找 name/desc 精确匹配的方法节点（仅编译输入自有方法）。
const jbytecode::tree::MethodNode* findOwnMethodNode(CompilerIndex* classIndex,
                                                     const ca::str::Utf8StringRef& owner,
                                                     const ca::str::Utf8StringRef& name,
                                                     const ca::str::Utf8StringRef& desc)
{
    if (classIndex == nullptr) return nullptr;
    CompilerClass* cls = classIndex->getClass(owner.to_std_string());
    if (cls == nullptr || cls->classNode == nullptr) return nullptr;
    for (const auto& member : cls->methods) {
        if (member.methodNode == nullptr) continue;
        if (member.methodNode->name == name && member.methodNode->desc == desc) {
            return member.methodNode;
        }
    }
    return nullptr;
}

/// javac 合成 values() 形态验证：getstatic owner.$VALUES:[LX; →
/// invokevirtual java/lang/Object.clone → checkcast [LX; → areturn。
/// 非该形态（手工字节码自定义 values()）时不允许 intrinsic 化。
bool isJavacEnumValuesBody(const jbytecode::tree::MethodNode* method,
                           const ca::str::Utf8StringRef& owner,
                           const ca::str::Utf8StringRef& arrayDesc)
{
    if (method == nullptr) return false;
    const auto* n = nextRealInsn(method->instructions.getFirst());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::FIELD_INSN ||
        n->getOpcode() != Opcodes::GETSTATIC) {
        return false;
    }
    const auto* field = static_cast<const jbytecode::tree::FieldInsnNode*>(n);
    if (!(field->owner == owner && field->name == "$VALUES" && field->desc == arrayDesc)) {
        return false;
    }
    n = nextRealInsn(n->getNext());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN ||
        n->getOpcode() != Opcodes::INVOKEVIRTUAL) {
        return false;
    }
    const auto* clone = static_cast<const jbytecode::tree::MethodInsnNode*>(n);
    // clone owner：javac 对数组 clone 发射数组类型描述符（"[LX;"）；
    // 同时接受 java/lang/Object（手工/旧编译器形态），desc 恒为
    // ()Ljava/lang/Object;。
    if (!(clone->name == "clone" && clone->desc == "()Ljava/lang/Object;" &&
          (clone->owner == arrayDesc || clone->owner == "java/lang/Object"))) {
        return false;
    }
    n = nextRealInsn(n->getNext());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::TYPE_INSN ||
        n->getOpcode() != Opcodes::CHECKCAST) {
        return false;
    }
    if (static_cast<const jbytecode::tree::TypeInsnNode*>(n)->desc != arrayDesc) return false;
    n = nextRealInsn(n->getNext());
    return n != nullptr && n->getType() == jbytecode::tree::AbstractInsnNode::INSN &&
           n->getOpcode() == Opcodes::ARETURN;
}

/// javac 合成 valueOf(String) 形态验证：ldc Lowner; → aload 0 →
/// invokestatic java/lang/Enum.valueOf(Ljava/lang/Class;Ljava/lang/String;)
/// → checkcast owner → areturn。
bool isJavacEnumValueOfBody(const jbytecode::tree::MethodNode* method,
                            const ca::str::Utf8StringRef& owner)
{
    if (method == nullptr) return false;
    const auto* n = nextRealInsn(method->instructions.getFirst());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::LDC_INSN ||
        n->getOpcode() != Opcodes::LDC) {
        return false;
    }
    const auto* ldc = static_cast<const jbytecode::tree::LdcInsnNode*>(n);
    const auto* ldcType = ldc->cst.as<jbytecode::core::Type>();
    // #921 staticArena 退役：仅瞬时比对，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    if (ldcType == nullptr ||
        ldcType->getDescriptor(scratch).to_std_string() != "L" + owner.to_std_string() + ";") {
        return false;
    }
    n = nextRealInsn(n->getNext());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::VAR_INSN ||
        n->getOpcode() != Opcodes::ALOAD ||
        static_cast<const jbytecode::tree::VarInsnNode*>(n)->var != 0) {
        return false;
    }
    n = nextRealInsn(n->getNext());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::METHOD_INSN ||
        n->getOpcode() != Opcodes::INVOKESTATIC) {
        return false;
    }
    const auto* valueOf = static_cast<const jbytecode::tree::MethodInsnNode*>(n);
    if (!(valueOf->owner == "java/lang/Enum" && valueOf->name == "valueOf" &&
          valueOf->desc == "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;")) {
        return false;
    }
    n = nextRealInsn(n->getNext());
    if (n == nullptr || n->getType() != jbytecode::tree::AbstractInsnNode::TYPE_INSN ||
        n->getOpcode() != Opcodes::CHECKCAST ||
        static_cast<const jbytecode::tree::TypeInsnNode*>(n)->desc != owner) {
        return false;
    }
    n = nextRealInsn(n->getNext());
    return n != nullptr && n->getType() == jbytecode::tree::AbstractInsnNode::INSN &&
           n->getOpcode() == Opcodes::ARETURN;
}

/// javac 合成 values() 调用点 intrinsic：getstatic $VALUES（cached field）+
/// 站点缓存 clone。替代 JNI 静态调用进 trampoline（其内部为 PushLocalFrame +
/// getstatic + owner-method clone + checkcast + PopLocalFrame）——每次调用
/// 约 5 个 JNI 降为 2 个。
bool tryEmitEnumValuesCall(CSourceStream& out, const RegInstruction& ins,
                           RegCFunctionEmitState& state)
{
    if (ins.method.invokeOpcode != Opcodes::INVOKESTATIC) return false;
    if (ins.method.name.to_std_string() != "values") return false;
    const std::string desc = ins.method.desc.to_std_string();
    if (desc.rfind("()[L", 0) != 0 || desc.back() != ';') return false;
    // 无缓存池（单测环境）没有 TU 级站点声明，走旧路径。
    if (state.cachePool == nullptr) return false;
    if (!isEnumSubtypeOwner(state.classIndex, ins.method.owner)) return false;
    const std::string arrayDesc = desc.substr(2);   // desc 去掉 "()" 前缀
    const auto arrayDescRef     = ca::str::Utf8StringRef::from_string_view(arrayDesc);
    // 预扫描集合优先（方法注入 native 后指令已清空）；单测直调发射器时回退
    // 现场验证。
    const std::string key = ins.method.owner.to_std_string() + "\n" + "values" + "\n" + desc;
    const bool verified =
        state.enumSyntheticMethods != nullptr
            ? state.enumSyntheticMethods->count(key) != 0
            : isJavacEnumValuesBody(
                  findOwnMethodNode(state.classIndex, ins.method.owner, ins.method.name,
                                    ins.method.desc),
                  ins.method.owner, arrayDescRef);
    if (!verified) return false;
    const ca::str::Utf8StringRef fieldCache = state.cachePool->staticFieldCache(
        ins.method.owner, ca::str::Utf8StringRef::from_cstr("$VALUES"), arrayDescRef);
    const ca::str::Utf8StringRef site = state.cachePool->enumValuesSite(ins.method.owner);
    if (!ins.hasDest) return false;   // values() 必有返回值；防御畸形 IR
    const std::string loadValues =
        codegen::exprCall("_mj2x_regc_load_cached_ref_field", {"&" + fieldCache.to_std_string()});
    out.appendLineStart(regName(ins.dest.id))
        .append(" = ")
        .append(codegen::exprCall("mj2x_intrinsic_enum_values",
                                  {"_mj2x_regc_current_context()", loadValues,
                                   "&" + site.to_std_string()}))
        .append(";")
        .newLine();
    return true;
}

/// javac 合成 valueOf(String) 调用点 intrinsic：Enum.valueOf(owner, name)
/// 直调（JDK enumConstantDirectory 映射）。替代 JNI 静态调用进合成 valueOf
/// trampoline（内部为 PushLocalFrame + 类字面量 + Enum.valueOf + checkcast +
/// PopLocalFrame）——约 5 个 JNI 降为 1 个。
bool tryEmitEnumValueOfCall(CSourceStream& out, const RegInstruction& ins,
                            RegCFunctionEmitState& state)
{
    if (ins.method.invokeOpcode != Opcodes::INVOKESTATIC) return false;
    if (ins.method.name.to_std_string() != "valueOf") return false;
    if (ins.method.desc.to_std_string().rfind("(Ljava/lang/String;)L", 0) != 0) return false;
    if (ins.args.empty()) return false;
    // 无缓存池（单测环境）没有 TU 级站点声明，走旧路径。
    if (state.cachePool == nullptr) return false;
    if (!isEnumSubtypeOwner(state.classIndex, ins.method.owner)) return false;
    const std::string descStr = ins.method.desc.to_std_string();
    // 预扫描集合优先（方法注入 native 后指令已清空）；单测直调发射器时回退
    // 现场验证。
    const std::string key =
        ins.method.owner.to_std_string() + "\n" + "valueOf" + "\n" + descStr;
    const bool verified =
        state.enumSyntheticMethods != nullptr
            ? state.enumSyntheticMethods->count(key) != 0
            : isJavacEnumValueOfBody(
                  findOwnMethodNode(state.classIndex, ins.method.owner, ins.method.name,
                                    ins.method.desc),
                  ins.method.owner);
    if (!verified) return false;
    const ca::str::Utf8StringRef site = state.cachePool->enumValueOfSite(ins.method.owner);
    if (!ins.hasDest) return false;
    out.appendLineStart(regName(ins.dest.id))
        .append(" = ")
        .append(codegen::exprCall("mj2x_intrinsic_enum_valueof",
                                  {"_mj2x_regc_current_context()",
                                   operandExpr(ins.args[0]).to_std_string(),
                                   "&" + site.to_std_string()}))
        .append(";")
        .newLine();
    return true;
}

/// 枚举子类调用点的 name()/ordinal() 直连发射。
///
/// @details
/// Enum.name()/ordinal() 是 final，任何枚举子类的同名调用点与
/// java/lang/Enum 精确等价；registry 按 owner 精确匹配（owner 是枚举子类
/// 二进制名）匹配不到，这里按类层级补匹配。发射为 NULL 检查 + 字段读取
/// intrinsic（GetObjectField/GetIntField，缓存 jfieldID），替代 resolved
/// dispatch 的 IsInstanceOf + CallXxxMethodA——每调用点 2 个 JNI 降为 1 个。
/// 不匹配时返回 false，调用方继续走 registry/通用路径。
bool tryEmitEnumMemberAccess(CSourceStream& out, const RegInstruction& ins,
                             RegCFunctionEmitState& state)
{
    if (ins.method.invokeOpcode != Opcodes::INVOKEVIRTUAL) return false;
    if (ins.args.empty()) return false;
    const std::string name = ins.method.name.to_std_string();
    const bool        isName =
        name == "name" && ins.method.desc.to_std_string() == "()Ljava/lang/String;";
    const bool isOrdinal = name == "ordinal" && ins.method.desc.to_std_string() == "()I";
    if (!isName && !isOrdinal) return false;
    if (!isEnumSubtypeOwner(state.classIndex, ins.method.owner)) return false;
    const std::string receiverExpr = operandExpr(ins.args[0]).to_std_string();
    const std::string destName     = ins.hasDest ? regName(ins.dest.id) : std::string();
    const char*       symbol       = isName ? "mj2x_intrinsic_enum_name"
                                            : "mj2x_intrinsic_enum_ordinal";
    out.iff(receiverExpr + " == NULL",
            [&](CSourceStream& u) {
                u.emitLine(std::string("_mj2x_regc_throw_null(\"") +
                           (isName ? "Enum.name" : "Enum.ordinal") + "\");");
                if (ins.hasDest) {
                    u.emitLine(destName + " = " + std::string(defaultValue(ins.dest.type)) + ";");
                }
            },
            [&](CSourceStream& u) {
                if (ins.hasDest) u.appendLineStart(destName).append(" = ");
                u.append(codegen::exprCall(symbol,
                                           {"_mj2x_regc_current_context()", receiverExpr}));
                u.append(";").newLine();
            });
    return true;
}

RegCIntrinsicDecision emitIntrinsicCall(CSourceStream& out,
                                        const RegInstruction& ins, RegCFunctionEmitState& state)
{
    const RegCIntrinsicRegistry& intrinsicRegistry =
        state.intrinsicRegistry == nullptr ? RegCIntrinsicRegistry::builtins()
                                           : *state.intrinsicRegistry;
    RegCIntrinsicDecision decision = intrinsicRegistry.matchCall(ins);
    if (decision.kind == RegCIntrinsicDecisionKind::Fallback &&
        (tryEmitEnumMemberAccess(out, ins, state) ||
         tryEmitEnumValuesCall(out, ins, state) ||
         tryEmitEnumValueOfCall(out, ins, state))) {
        decision.kind = RegCIntrinsicDecisionKind::Emit;
        decision.reason.clear();
        return decision;
    }
    if (decision.kind != RegCIntrinsicDecisionKind::Emit) return decision;

    if (decision.emitKind == RegCIntrinsicEmitKind::WrapperValueOf) {
        if (!ins.hasDest) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "wrapper valueOf intrinsic missing destination";
            return decision;
        }
        if (state.nativeWrapperRegisters.count(ins.dest.id.value) == 0) {
            // 逃逸链：走 runtime 小值缓存装箱（Integer/Long/Short/Byte 对
            // [-128,127]、Character 对 [0,127]、Boolean 对 TRUE/FALSE 命中
            // 0 JNI，返回对象与 JDK valueOf 逐次调用恒等；越界值透传 JDK
            // 调用）。Float/Double 无 JDK 缓存语义，保持通用 invokestatic 回退。
            static const std::unordered_map<RegCIntrinsicNativeObjectKind, std::pair<const char*, const char*>>
                kEscapingBoxSymbols = {
                    {RegCIntrinsicNativeObjectKind::Int, {"_mj2x_regc_box_i32", "int32_t"}},
                    {RegCIntrinsicNativeObjectKind::Long, {"_mj2x_regc_box_i64", "int64_t"}},
                    {RegCIntrinsicNativeObjectKind::Short, {"_mj2x_regc_box_i16", "int16_t"}},
                    {RegCIntrinsicNativeObjectKind::Byte, {"_mj2x_regc_box_i8", "int8_t"}},
                    {RegCIntrinsicNativeObjectKind::Bool, {"_mj2x_regc_box_bool", "bool"}},
                    {RegCIntrinsicNativeObjectKind::Char, {"_mj2x_regc_box_char", "uint16_t"}},
                };
            const auto boxIt = kEscapingBoxSymbols.find(decision.nativeObjectKind);
            if (boxIt != kEscapingBoxSymbols.end() && !ins.args.empty()) {
                const ca::str::Utf8String argExpr = operandExpr(ins.args[0]);
                out.appendLineStart(regName(ins.dest.id))
                    .append(" = (mj2x_ref)")
                    .append(codegen::exprCall(
                        boxIt->second.first,
                        {codegen::exprCast(boxIt->second.second,
                                           codegen::exprParen(argExpr.to_std_string()))}))
                    .append(";")
                    .newLine();
                return decision;
            }
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
        // 发射端预检位（#879 Y4 解耦）：预检判定随权威表（kEntries 的 precheck
        // 位）单源，此处按位发射——Random.nextInt(bound)：JVM 对 bound<=0 抛
        // IllegalArgumentException；crt 直调无异常通道（bound<=0 返回哨兵值），
        // 预检把异常挂到 context——pending 由 mayThrow 的调用后异常分发（有 try
        // 区域）或 JNI 边界（无区域）统一处理（fail-closed）。
        if (decision.precheck == RegCIntrinsicPrecheck::PositiveBound) {
            out.emitLine("if (" + operandExpr(ins.args[1]).to_std_string() + " <= 0) {");
            out.indent();
            out.emitLine("mj2x_regc_throw_exception(_mj2x_regc_current_context(), "
                         "\"java/lang/IllegalArgumentException\", \"bound must be positive\");");
            if (ins.hasDest) {
                out.emitLine(regName(ins.dest.id) + " = 0;");
            }
            out.emitLine("} else {");
            out.indent();
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
        out.append(";");
        if (decision.precheck == RegCIntrinsicPrecheck::PositiveBound) {
            out.newLine();
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.newLine();
        }
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

    if (decision.emitKind == RegCIntrinsicEmitKind::UserIntrinsic) {
        // 用户自定义 intrinsic（issue #818）：直接调用户 symbol。
        // 参数顺序（三后端一致）：passContext 时第一参数 JNIEnv* env；
        // passReceiver（instance 调用点）时 receiver 紧随 env——RegIR instance
        // 调用 args[0] 即 receiver，此处按序透传全部 args 即为契约顺序。
        std::vector<std::string> userArgs;
        if (decision.passContext) userArgs.push_back("_mj2x_regc_current_context()->env");
        // JVM 语义保持：instance 调用 receiver 为 NULL 时抛 NPE（与通用 JNI
        // 派发路径的 emitCachedCall 一致），不把 NULL 静默递给用户实现。
        // ThrowNew 仅挂 pending 异常后正常返回，必须 goto cleanup 终止本调用
        // 路径，否则 NULL receiver 仍会递给用户 C 实现（F1 直派同款写法，
        // 见 emitIndyDirectFieldCall）。
        if (decision.passReceiver && !ins.args.empty()) {
            const std::string receiverExpr = operandExpr(ins.args[0]).to_std_string();
            out.iff(receiverExpr + " == NULL",
                    [&](CSourceStream& u) {
                        u.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                        u.goto_("_mj2x_regc_cleanup");
                    });
        }
        for (const auto& arg : ins.args) {
            userArgs.push_back(operandExpr(arg).to_std_string());
        }
        out.appendLineStart();
        if (ins.hasDest) {
            out.append(regName(ins.dest.id)).append(" = ");
        }
        out.append(codegen::exprCallArgs(decision.symbol, userArgs));
        out.append(";").newLine();
        // 用户实现可能经 JNI 抛异常：恒保留 pending 异常检查（不做 pure 豁免）。
        out.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
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

#include "regc_emitter.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;

namespace {

// cStringLiteral is shared with StackC via codegen::cStringLiteral; alias it
// here so the existing call sites stay one-line.
using codegen::cStringLiteral;

const char* cType(SSAType type)
{
    switch (type) {
    case SSAType::VOID: return "void";
    case SSAType::BOOL: return "bool";
    case SSAType::I8: return "int8_t";
    case SSAType::I16: return "int16_t";
    case SSAType::CHAR: return "uint16_t";
    case SSAType::I32: return "int32_t";
    case SSAType::I64: return "int64_t";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::REF: return "void*";
    default: return "int32_t";
    }
}

const char* unsignedCType(SSAType type)
{
    switch (type) {
    case SSAType::I64: return "uint64_t";
    default: return "uint32_t";
    }
}

bool isIntegerLike(SSAType type)
{
    return ssaIsIntLike(type) || type == SSAType::I64;
}

std::string sanitizeIdent(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
        }
        else {
            out.push_back('_');
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string functionName(const SSAFunctionSymbol& symbol)
{
    return "regc_" + sanitizeIdent(symbol.owner) + "_" + sanitizeIdent(symbol.name) + "_" +
           sanitizeIdent(symbol.desc);
}

std::string regName(RegId id)
{
    return "r" + std::to_string(id.value);
}

const char* defaultValue(SSAType type)
{
    switch (type) {
    case SSAType::VOID: return "";
    case SSAType::F32: return "0.0f";
    case SSAType::F64: return "0.0";
    case SSAType::REF: return "NULL";
    case SSAType::BOOL: return "false";
    default: return "0";
    }
}

const char* helperSuffix(SSAType type)
{
    switch (type) {
    case SSAType::VOID: return "void";
    case SSAType::BOOL:
    case SSAType::I8:
    case SSAType::I16:
    case SSAType::CHAR:
    case SSAType::I32: return "i32";
    case SSAType::I64: return "i64";
    case SSAType::F32: return "f32";
    case SSAType::F64: return "f64";
    case SSAType::REF: return "ref";
    default: return "i32";
    }
}

std::string operandExpr(const RegOperand& operand)
{
    std::ostringstream os;
    switch (operand.kind) {
    case RegOperandKind::Register: return regName(operand.reg);
    case RegOperandKind::IntLiteral:
        if (operand.type == SSAType::I64) {
            os << "(int64_t)" << operand.intValue << "LL";
        }
        else {
            os << operand.intValue;
        }
        return os.str();
    case RegOperandKind::FloatLiteral:
        os << operand.floatValue;
        if (operand.type == SSAType::F32) os << "f";
        return os.str();
    case RegOperandKind::NullLiteral: return "NULL";
    case RegOperandKind::Opaque:
        // A String LDC carries its UTF-8 content after "ldc_string:" so the
        // String intrinsic path can materialize the jstring without going
        // through JNI call dispatch.  Other opaques stay as annotated NULL.
        if (operand.tag.rfind("ldc_string:", 0) == 0) {
            return "_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), " +
                   cStringLiteral(operand.tag.substr(std::string("ldc_string:").size())) + ")";
        }
        return "NULL /* opaque:" + operand.tag + " */";
    }
    return "0";
}

const char* binaryOp(SSABinaryOp op)
{
    switch (op) {
    case SSABinaryOp::Add: return "+";
    case SSABinaryOp::Sub: return "-";
    case SSABinaryOp::Mul: return "*";
    case SSABinaryOp::Div: return "/";
    case SSABinaryOp::Rem: return "%";
    case SSABinaryOp::And: return "&";
    case SSABinaryOp::Or: return "|";
    case SSABinaryOp::Xor: return "^";
    case SSABinaryOp::Shl: return "<<";
    case SSABinaryOp::Shr: return ">>";
    case SSABinaryOp::UShr: return ">>";
    }
    return "+";
}

const char* compareOp(SSAComparePredicate predicate)
{
    switch (predicate) {
    case SSAComparePredicate::Eq: return "==";
    case SSAComparePredicate::Ne: return "!=";
    case SSAComparePredicate::Lt: return "<";
    case SSAComparePredicate::Le: return "<=";
    case SSAComparePredicate::Gt: return ">";
    case SSAComparePredicate::Ge: return ">=";
    }
    return "==";
}

bool collectRegister(std::map<uint32_t, SSAType>& regs, RegValue value)
{
    auto [it, inserted] = regs.emplace(value.id.value, value.type);
    return inserted || it->second == value.type;
}

void collectCopies(std::map<uint32_t, SSAType>& regs, const std::vector<RegCopy>& copies)
{
    for (const auto& copy : copies) collectRegister(regs, copy.dest);
}

const char* fieldHelperSuffix(const std::string& desc, SSAType fallback)
{
    // JVM narrow fields are int-like on the operand stack but require their
    // descriptor-specific JNI accessors at the native boundary.
    if (!desc.empty()) {
        switch (desc[0]) {
        case 'Z': return "bool";
        case 'B': return "i8";
        case 'C': return "char";
        case 'S': return "i16";
        case 'I': return "i32";
        case 'J': return "i64";
        case 'F': return "f32";
        case 'D': return "f64";
        default: return "ref";
        }
    }
    return helperSuffix(fallback);
}

const char* arrayHelperSuffix(SSAType type)
{
    // BALOAD/BASTORE cover both byte[] and boolean[]; the i8 runtime helper
    // performs the final array-class check when it sees the actual reference.
    switch (type) {
    case SSAType::BOOL:
    case SSAType::I8: return "i8";
    case SSAType::I16: return "i16";
    case SSAType::CHAR: return "char";
    default: return helperSuffix(type);
    }
}

bool isReferenceArrayDesc(const std::string& desc)
{
    return desc.size() > 1 && desc[0] == '[' && (desc[1] == 'L' || desc[1] == '[');
}

std::string arrayComponentDesc(const std::string& desc)
{
    if (desc.size() <= 1) return desc;
    std::string component = desc.substr(1);
    if (component.size() >= 2 && component.front() == 'L' && component.back() == ';') {
        return component.substr(1, component.size() - 2);
    }
    return component;
}

std::vector<std::string> multiArrayComponentDescs(const std::string& desc, size_t dimCount)
{
    std::vector<std::string> components;
    std::string current = desc;
    for (size_t i = 0; i < dimCount && isReferenceArrayDesc(current); ++i) {
        components.push_back(arrayComponentDesc(current));
        current = current.substr(1);
    }
    return components;
}

bool isStaticCall(const RegInstruction& ins)
{
    return ins.method.invokeOpcode == Opcodes::INVOKESTATIC;
}

void collectExceptionRegisters(std::map<uint32_t, SSAType>&           regs,
                               const std::vector<RegExceptionRegion>& regions)
{
    for (const auto& region : regions) collectRegister(regs, region.exceptionValue);
}

void collectTerminatorRegisters(std::map<uint32_t, SSAType>& regs, const RegTerminator& terminator)
{
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                collectCopies(regs, t.target.copies);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                collectCopies(regs, t.trueTarget.copies);
                collectCopies(regs, t.falseTarget.copies);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                for (const auto& c : t.cases) collectCopies(regs, c.target.copies);
                collectCopies(regs, t.defaultTarget.copies);
            }
        },
        terminator);
}

void collectEdgeCopyTempTypes(std::vector<SSAType>& temps, const std::vector<RegCopy>& copies)
{
    for (const auto& copy : copies) temps.push_back(copy.dest.type);
}

void collectTerminatorEdgeCopyTempTypes(std::vector<SSAType>& temps,
                                        const RegTerminator&  terminator)
{
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                collectEdgeCopyTempTypes(temps, t.target.copies);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                collectEdgeCopyTempTypes(temps, t.trueTarget.copies);
                collectEdgeCopyTempTypes(temps, t.falseTarget.copies);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                for (const auto& c : t.cases) collectEdgeCopyTempTypes(temps, c.target.copies);
                collectEdgeCopyTempTypes(temps, t.defaultTarget.copies);
            }
        },
        terminator);
}

std::vector<SSAType> collectEdgeCopyTempTypes(const RegFunction& fn)
{
    std::vector<SSAType> temps;
    for (const auto& block : fn.blocks) {
        collectTerminatorEdgeCopyTempTypes(temps, block->terminator);
    }
    return temps;
}

bool collectRegisters(const RegFunction& fn, std::map<uint32_t, SSAType>& regs, std::string& reason)
{
    for (const auto& parameter : fn.parameters) collectRegister(regs, parameter);
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && !collectRegister(regs, ins.dest)) {
                reason = "conflicting RegIR destination type";
                return false;
            }
            if (ins.kind == RegInstructionKind::Copy) collectRegister(regs, ins.copy.dest);
        }
        collectTerminatorRegisters(regs, block->terminator);
    }
    collectExceptionRegisters(regs, fn.exceptionRegions);
    return true;
}

struct RegCNativeObjectState
{
    RegCIntrinsicNativeObjectKind kind = RegCIntrinsicNativeObjectKind::None;
    std::string                   storageName;
};

struct RegCFunctionEmitState
{
    std::map<uint32_t, RegCNativeObjectState> nativeObjects;
    std::string                               functionName;
    SSAFunctionSymbol                         symbol;
    RegCCachePool*                            cachePool = nullptr;
    std::vector<std::string>                  staticMethodCacheDeclarations;
    std::vector<std::string>                  staticFieldCacheDeclarations;
    std::vector<std::string>                  classCacheDeclarations;
    std::map<std::string, std::string>        staticMethodCacheNames;
    std::map<std::string, std::string>        staticFieldCacheNames;
    std::map<std::string, std::string>        classCacheNames;
    int                                       staticMethodCacheIndex = 0;
    int                                       staticFieldCacheIndex  = 0;
    int                                       classCacheIndex        = 0;
    int                                       stringBuilderTempIndex = 0;
    std::set<uint32_t>                        nativeStringBuilderRegisters;
    std::vector<uint32_t>                     nativeStringBuilderOwners;
    std::map<uint32_t, uint32_t>              nativeStringBuilderOwnerByRegister;
    std::set<uint32_t>                        nativeWrapperRegisters;
};

const char* nativeObjectType(RegCIntrinsicNativeObjectKind kind)
{
    switch (kind) {
    case RegCIntrinsicNativeObjectKind::Int: return "mj2x_int";
    case RegCIntrinsicNativeObjectKind::Long: return "mj2x_long";
    case RegCIntrinsicNativeObjectKind::Short: return "mj2x_short";
    case RegCIntrinsicNativeObjectKind::Byte: return "mj2x_byte";
    case RegCIntrinsicNativeObjectKind::Float: return "mj2x_float";
    case RegCIntrinsicNativeObjectKind::Double: return "mj2x_double";
    case RegCIntrinsicNativeObjectKind::Bool: return "mj2x_bool";
    case RegCIntrinsicNativeObjectKind::Char: return "mj2x_char";
    case RegCIntrinsicNativeObjectKind::Random: return "mj2x_random";
    case RegCIntrinsicNativeObjectKind::StringBuilder: return "mj2x_string_builder";
    default: return "mj2x_ref";
    }
}

std::string stringBuilderOwnerName(RegId id)
{
    return "native_owner_r" + std::to_string(id.value);
}

std::string stringBuilderMaterializedName(RegId id)
{
    return "native_materialized_r" + std::to_string(id.value);
}

std::string stringBuilderHandleExpr(const RegOperand& operand)
{
    return "(mj2x_string_builder**)" + operandExpr(operand);
}

std::string stringBuilderStateExpr(const RegOperand& operand)
{
    return "*(" + stringBuilderHandleExpr(operand) + ")";
}

void emitRegCStatusCheck(std::ostringstream& out, RegCFunctionEmitState& state,
                         const std::string& statusExpr, const std::string& operation)
{
    const int temp = state.stringBuilderTempIndex++;
    out << "    mj2x_regc_status runtime_status_" << temp << " = " << statusExpr << ";\n";
    out << "    if (runtime_status_" << temp
        << " != MJ2X_REGC_STATUS_OK && runtime_status_" << temp
        << " != MJ2X_REGC_STATUS_PENDING_EXCEPTION && !_mj2x_regc_exception_check())\n";
    out << "        mj2x_regc_report_status(_mj2x_regc_current_context(), runtime_status_" << temp
        << ", " << cStringLiteral(operation) << ");\n";
}

std::string nativeObjectStorageName(RegId id)
{
    return "native_r" + std::to_string(id.value);
}

const RegCNativeObjectState* nativeObjectFor(const RegCFunctionEmitState& state,
                                             const RegOperand&            operand)
{
    if (operand.kind != RegOperandKind::Register) return nullptr;
    auto it = state.nativeObjects.find(operand.reg.value);
    if (it == state.nativeObjects.end()) return nullptr;
    return &it->second;
}

bool emitMaterializedStringBuilder(std::ostringstream& out, RegCFunctionEmitState& state,
                                   const RegOperand& operand, std::string& expression)
{
    const RegCNativeObjectState* objectState = nativeObjectFor(state, operand);
    if (objectState == nullptr ||
        objectState->kind != RegCIntrinsicNativeObjectKind::StringBuilder) {
        expression = operandExpr(operand);
        return false;
    }
    if (operand.kind != RegOperandKind::Register) {
        expression = operandExpr(operand);
        return false;
    }
    const auto ownerIt =
        state.nativeStringBuilderOwnerByRegister.find(operand.reg.value);
    if (ownerIt == state.nativeStringBuilderOwnerByRegister.end()) {
        expression = operandExpr(operand);
        return false;
    }

    const RegId owner{ownerIt->second};
    expression = stringBuilderMaterializedName(owner);
    out << "    if (" << expression
        << " == NULL && !_mj2x_regc_exception_check()) {\n";
    emitRegCStatusCheck(
        out,
        state,
        "mj2x_string_builder_materialize(_mj2x_regc_current_context(), " +
            stringBuilderStateExpr(operand) + ", &" + expression + ")",
        "StringBuilder.materialize");
    out << "    }\n";
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

struct RegCStringBuilderPlan
{
    std::set<uint32_t>    nativeRegisters;
    std::vector<uint32_t> ownerRegisters;
    std::map<uint32_t, uint32_t> ownerByRegister;
};

class RegIdUnionFind
{
public:
    void add(uint32_t id)
    {
        parent_.emplace(id, id);
    }

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

    const std::map<uint32_t, uint32_t>& entries() const
    {
        return parent_;
    }

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
            using T = std::decay_t<decltype(t)>;
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

RegCStringBuilderPlan analyzeStringBuilders(const RegFunction& fn)
{
    RegIdUnionFind       ids;
    std::vector<uint32_t> allocations;

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
                     ins.typeDesc == "java/lang/StringBuilder") {
                allocations.push_back(ins.dest.id.value);
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

    std::set<uint32_t> builderRoots;
    for (uint32_t allocation : allocations) builderRoots.insert(ids.find(allocation));

    auto builderRoot = [&](const RegOperand& operand, uint32_t& root) {
        uint32_t id = 0;
        if (!referenceRegister(operand, id)) return false;
        root = ids.find(id);
        return builderRoots.count(root) != 0;
    };
    std::set<uint32_t> fallbackRoots;
    auto fallback = [&](const RegOperand& operand) {
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
                           const RegOperand& operand, const RegBlock* block, size_t index) {
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
                                      decision.emitKind ==
                                          RegCIntrinsicEmitKind::StringBuilderInit;
                const bool safeAppend = decision.kind == RegCIntrinsicDecisionKind::Emit &&
                                        decision.emitKind ==
                                            RegCIntrinsicEmitKind::StringBuilderAppend;
                const bool safeMethod = decision.kind == RegCIntrinsicDecisionKind::Emit &&
                                        decision.emitKind ==
                                            RegCIntrinsicEmitKind::NativeObjectMethod &&
                                        decision.nativeObjectKind ==
                                            RegCIntrinsicNativeObjectKind::StringBuilder;
                if (safeInit || safeAppend || safeMethod) {
                    if (!ins.args.empty()) {
                        recordEvent(
                            nativeUseEvents, ins.args[0], block.get(), instructionIndex);
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
                        recordEvent(escapeEvents,
                                    t.value,
                                    block.get(),
                                    block->instructions.size());
                    }
                }
            },
            block->terminator);
    }

    std::map<uint32_t, size_t> allocationCounts;
    std::map<uint32_t, uint32_t> ownerByRoot;
    for (uint32_t allocation : allocations) {
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
            std::set<const RegBlock*> visited;
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
        }
    }
    for (uint32_t allocation : allocations) {
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
    std::set<uint32_t> fallbackRoots;
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.kind != RegInstructionKind::Call || !ins.hasDest ||
                ins.dest.type != SSAType::REF) {
                continue;
            }
            const RegCIntrinsicDecision decision =
                RegCIntrinsicRegistry::builtins().matchCall(ins);
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
                    decision.emitKind == RegCIntrinsicEmitKind::WrapperUnbox &&
                    !ins.args.empty()) {
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

bool isSupportedInstruction(const RegInstruction& ins, std::string& reason)
{
    switch (ins.kind) {
    case RegInstructionKind::Copy:
    case RegInstructionKind::Unary:
    case RegInstructionKind::Binary:
    case RegInstructionKind::Compare:
    case RegInstructionKind::Convert:
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier: return true;
    }
    reason = "unknown RegIR instruction kind";
    return false;
}

std::string binaryExpr(const RegInstruction& ins)
{
    const std::string lhs  = operandExpr(ins.lhs);
    const std::string rhs  = operandExpr(ins.rhs);
    const SSAType     type = ins.dest.type;

    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Div || ins.binaryOp == SSABinaryOp::Rem)) {
        const char* op    = ins.binaryOp == SSABinaryOp::Div ? "div" : "rem";
        const char* width = type == SSAType::I64 ? "i64" : "i32";
        return "mj2x_regc_" + std::string(op) + "_" + width + "(_mj2x_regc_current_context(), " +
               lhs + ", " + rhs + ")";
    }
    if (ins.binaryOp == SSABinaryOp::UShr) {
        return "(" + std::string(cType(type)) + ")((" + unsignedCType(type) + ")" + lhs + " >> " +
               rhs + ")";
    }
    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub ||
         ins.binaryOp == SSABinaryOp::Mul)) {
        return "(" + std::string(cType(type)) + ")((" + unsignedCType(type) + ")" + lhs + " " +
               binaryOp(ins.binaryOp) + " (" + unsignedCType(type) + ")" + rhs + ")";
    }
    if ((type == SSAType::F32 || type == SSAType::F64) && ins.binaryOp == SSABinaryOp::Rem) {
        return std::string(type == SSAType::F32 ? "fmodf" : "fmod") + "(" + lhs + ", " + rhs + ")";
    }
    return "(" + std::string(cType(type)) + ")(" + lhs + " " + binaryOp(ins.binaryOp) + " " + rhs +
           ")";
}

void emitHelperPrelude(std::ostream& out)
{
    out << "static mj2x_regc_context* _mj2x_regc_current_context(void);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_ref_array_cached(mj2x_regc_class_cache* "
           "component_cache, int32_t length);\n";
    out << "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, "
           "int32_t* dims);\n";
    out << "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref);\n";
    out << "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc);\n";
    out << "static mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache);\n";
    out << "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc);\n";
    out << "static int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, "
           "mj2x_regc_class_cache* cache);\n";
    out << "static void _mj2x_regc_monitor_enter(mj2x_ref value);\n";
    out << "static void _mj2x_regc_monitor_exit(mj2x_ref value);\n";
    out << "static int32_t _mj2x_regc_exception_check(void);\n";
    out << "static mj2x_ref _mj2x_regc_exception_occurred(void);\n";
    out << "static void _mj2x_regc_exception_clear(void);\n";
    out << "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type);\n";
    out << "static void _mj2x_regc_throw(mj2x_ref value);\n";
    out << "static bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc);\n";
    out << "static bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* "
           "cache);\n";
    out << "static int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache);\n";
    out << "static mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* "
           "cache);\n";
    out << "static bool _mj2x_regc_load_cached_instance_bool_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static int8_t _mj2x_regc_load_cached_instance_i8_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static uint16_t _mj2x_regc_load_cached_instance_char_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static int16_t _mj2x_regc_load_cached_instance_i16_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static int32_t _mj2x_regc_load_cached_instance_i32_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static int64_t _mj2x_regc_load_cached_instance_i64_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static float _mj2x_regc_load_cached_instance_f32_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static double _mj2x_regc_load_cached_instance_f64_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static mj2x_ref _mj2x_regc_load_cached_instance_ref_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n";
    out << "static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, bool value);\n";
    out << "static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, int8_t value);\n";
    out << "static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, uint16_t value);\n";
    out << "static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, int16_t value);\n";
    out << "static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, int32_t value);\n";
    out << "static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, int64_t value);\n";
    out << "static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, float value);\n";
    out << "static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, double value);\n";
    out << "static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const "
           "char* owner, const char* name, const char* desc, mj2x_ref value);\n";
    out << "static void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, "
           "bool value);\n";
    out << "static void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, "
           "int8_t value);\n";
    out << "static void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, "
           "uint16_t value);\n";
    out << "static void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, "
           "int16_t value);\n";
    out << "static void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, "
           "int32_t value);\n";
    out << "static void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, "
           "int64_t value);\n";
    out << "static void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, "
           "float value);\n";
    out << "static void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, "
           "double value);\n";
    out << "static void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, "
           "mj2x_ref value);\n";
    out << "static void _mj2x_regc_store_cached_instance_bool_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value);\n";
    out << "static void _mj2x_regc_store_cached_instance_i8_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value);\n";
    out << "static void _mj2x_regc_store_cached_instance_char_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value);\n";
    out << "static void _mj2x_regc_store_cached_instance_i16_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value);\n";
    out << "static void _mj2x_regc_store_cached_instance_i32_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value);\n";
    out << "static void _mj2x_regc_store_cached_instance_i64_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value);\n";
    out << "static void _mj2x_regc_store_cached_instance_f32_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value);\n";
    out << "static void _mj2x_regc_store_cached_instance_f64_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value);\n";
    out << "static void _mj2x_regc_store_cached_instance_ref_field("
           "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value);\n";
    out << "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index);\n";
    out << "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t "
           "value);\n";
    out << "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t "
           "value);\n";
    out << "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t "
           "value);\n";
    out << "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t "
           "value);\n";
    out << "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t "
           "value);\n";
    out << "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float "
           "value);\n";
    out << "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double "
           "value);\n";
    out << "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref "
           "value);\n";
    out << "static int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static float _mj2x_regc_call_f32(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static double _mj2x_regc_call_f64(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static void _mj2x_regc_call_void(const char* owner, const char* name, const char* "
           "desc, int32_t is_static, int32_t arg_count, ...);\n";
    out << "static int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, "
           "int32_t arg_count, ...);\n";
    out << "static int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, "
           "int32_t arg_count, ...);\n";
    out << "static float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t "
           "arg_count, ...);\n";
    out << "static double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t "
           "arg_count, ...);\n";
    out << "static mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, "
           "int32_t arg_count, ...);\n";
    out << "static void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t "
           "arg_count, ...);\n";
    out << "static int32_t _mj2x_regc_call_cached_instance_i32("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    out << "static int64_t _mj2x_regc_call_cached_instance_i64("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    out << "static float _mj2x_regc_call_cached_instance_f32("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    out << "static double _mj2x_regc_call_cached_instance_f64("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    out << "static mj2x_ref _mj2x_regc_call_cached_instance_ref("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    out << "static void _mj2x_regc_call_cached_instance_void("
           "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n";
    RegCIntrinsicRegistry::builtins().emitDeclarations(out);
}

std::string callArgs(const std::vector<RegOperand>& args)
{
    std::ostringstream os;
    os << static_cast<int32_t>(args.size());
    for (const auto& arg : args) {
        os << ", (" << cType(arg.type) << ")(" << operandExpr(arg) << ")";
    }
    return os.str();
}

std::string callArgs(const std::vector<RegOperand>& args,
                     const std::vector<std::string>& expressions)
{
    std::ostringstream os;
    os << static_cast<int32_t>(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        os << ", (" << cType(args[i].type) << ")(" << expressions[i] << ")";
    }
    return os.str();
}

std::string directCallArgs(const std::vector<RegOperand>& args)
{
    std::ostringstream os;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) os << ", ";
        os << "(" << cType(args[i].type) << ")(" << operandExpr(args[i]) << ")";
    }
    return os.str();
}

std::string directCallArgs(const std::vector<RegOperand>& args,
                           const std::vector<std::string>& expressions)
{
    std::ostringstream os;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) os << ", ";
        os << "(" << cType(args[i].type) << ")(" << expressions[i] << ")";
    }
    return os.str();
}

bool isSelfStaticCall(const RegCFunctionEmitState& state, const RegInstruction& ins)
{
    return isStaticCall(ins) && state.symbol.owner == ins.method.owner &&
           state.symbol.name == ins.method.name && state.symbol.desc == ins.method.desc;
}

std::string cacheKey(const std::string& first, const std::string& second, const std::string& third)
{
    return first + "\n" + second + "\n" + third;
}

std::string addStaticMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticMethodCache(
            ins.method.owner, ins.method.name, ins.method.desc);
    }

    const std::string key = cacheKey(ins.method.owner, ins.method.name, ins.method.desc);
    auto              it  = state.staticMethodCacheNames.find(key);
    if (it != state.staticMethodCacheNames.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_cache_" + state.functionName + "_" +
                                   std::to_string(state.staticMethodCacheIndex++);
    std::ostringstream decl;
    decl << "static mj2x_regc_static_method_cache " << cacheName << " = {"
         << cStringLiteral(ins.method.owner) << ", " << cStringLiteral(ins.method.name) << ", "
         << cStringLiteral(ins.method.desc) << ", NULL, NULL};\n";
    state.staticMethodCacheDeclarations.push_back(decl.str());
    state.staticMethodCacheNames.emplace(key, cacheName);
    return cacheName;
}

std::string addInstanceMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return "";
    return state.cachePool->instanceMethodCache(ins.method.owner, ins.method.name, ins.method.desc);
}

std::string addStaticFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
    }

    const std::string key = cacheKey(ins.field.owner, ins.field.name, ins.field.desc);
    auto              it  = state.staticFieldCacheNames.find(key);
    if (it != state.staticFieldCacheNames.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_field_cache_" + state.functionName + "_" +
                                   std::to_string(state.staticFieldCacheIndex++);
    std::ostringstream decl;
    decl << "static mj2x_regc_static_field_cache " << cacheName << " = {"
         << cStringLiteral(ins.field.owner) << ", " << cStringLiteral(ins.field.name) << ", "
         << cStringLiteral(ins.field.desc) << ", NULL, NULL};\n";
    state.staticFieldCacheDeclarations.push_back(decl.str());
    state.staticFieldCacheNames.emplace(key, cacheName);
    return cacheName;
}

std::string addInstanceFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return "";
    return state.cachePool->instanceFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
}

std::string addClassCache(RegCFunctionEmitState& state, const std::string& desc)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->classCache(desc);
    }

    auto it = state.classCacheNames.find(desc);
    if (it != state.classCacheNames.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_class_cache_" + state.functionName + "_" +
                                   std::to_string(state.classCacheIndex++);
    std::ostringstream decl;
    decl << "static mj2x_regc_class_cache " << cacheName << " = {" << cStringLiteral(desc)
         << ", NULL};\n";
    state.classCacheDeclarations.push_back(decl.str());
    state.classCacheNames.emplace(desc, cacheName);
    return cacheName;
}

RegCIntrinsicDecision emitIntrinsicCall(std::ostringstream& out, const RegInstruction& ins,
                                        RegCFunctionEmitState& state)
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
        out << "    " << nativeObjectType(decision.nativeObjectKind) << " "
            << nativeObjectStorageName(ins.dest.id) << " = " << decision.symbol << "(";
        if (!ins.args.empty()) out << operandExpr(ins.args[0]);
        out << ");\n";
        out << "    " << regName(ins.dest.id) << " = NULL;\n";
        state.nativeObjects[ins.dest.id.value] =
            RegCNativeObjectState{decision.nativeObjectKind, nativeObjectStorageName(ins.dest.id)};
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
        out << "    ";
        if (ins.hasDest) out << regName(ins.dest.id) << " = ";
        out << decision.symbol << "(" << objectState->storageName << ");\n";
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
        out << "    " << decision.symbol << "(&" << objectState->storageName << ", "
            << operandExpr(ins.args[1]) << ");\n";
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
        if (decision.nativeObjectKind == RegCIntrinsicNativeObjectKind::StringBuilder) {
            if (!ins.hasDest) {
                decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
                decision.reason = "StringBuilder method intrinsic missing destination";
                return decision;
            }
            const int valueTemp = state.stringBuilderTempIndex++;
            out << "    mj2x_ref sb_value_" << valueTemp << " = NULL;\n";
            emitRegCStatusCheck(
                out,
                state,
                decision.symbol + "(_mj2x_regc_current_context(), " +
                    stringBuilderStateExpr(ins.args[0]) + ", &sb_value_" +
                    std::to_string(valueTemp) + ")",
                "StringBuilder.toString");
            out << "    " << regName(ins.dest.id) << " = sb_value_" << valueTemp << ";\n";
            return decision;
        }
        out << "    ";
        if (ins.hasDest) out << regName(ins.dest.id) << " = ";
        out << decision.symbol << "(&" << objectState->storageName;
        for (size_t i = 1; i < ins.args.size(); ++i) {
            out << ", " << operandExpr(ins.args[i]);
        }
        out << ");\n";
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringBuilderInit) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "StringBuilder init intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != RegCIntrinsicNativeObjectKind::StringBuilder) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "StringBuilder receiver is not native state";
            return decision;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            if (nativeObjectFor(state, ins.args[i])) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "StringBuilder init argument is native state";
                return decision;
            }
        }
        std::ostringstream call;
        call << decision.symbol << "(_mj2x_regc_current_context(), ";
        if (ins.args.size() == 1) {
            call << stringBuilderHandleExpr(ins.args[0]);
        }
        else {
            call << operandExpr(ins.args[1]) << ", " << stringBuilderHandleExpr(ins.args[0]);
        }
        call << ")";
        emitRegCStatusCheck(out, state, call.str(), "StringBuilder.<init>");
        return decision;
    }

    if (decision.emitKind == RegCIntrinsicEmitKind::StringBuilderAppend) {
        if (ins.args.empty()) {
            decision.kind   = RegCIntrinsicDecisionKind::Unsupported;
            decision.reason = "StringBuilder append intrinsic missing receiver";
            return decision;
        }
        const RegCNativeObjectState* objectState = nativeObjectFor(state, ins.args[0]);
        if (!objectState || objectState->kind != RegCIntrinsicNativeObjectKind::StringBuilder) {
            decision.kind   = RegCIntrinsicDecisionKind::Fallback;
            decision.reason = "StringBuilder append receiver is not native state";
            return decision;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            if (nativeObjectFor(state, ins.args[i])) {
                decision.kind   = RegCIntrinsicDecisionKind::Fallback;
                decision.reason = "StringBuilder append argument is native state";
                return decision;
            }
        }
        std::ostringstream call;
        call << decision.symbol << "(_mj2x_regc_current_context(), "
             << stringBuilderStateExpr(ins.args[0]);
        for (size_t i = 1; i < ins.args.size(); ++i) {
            call << ", " << operandExpr(ins.args[i]);
        }
        call << ")";
        emitRegCStatusCheck(out, state, call.str(), "StringBuilder.append");
        if (ins.hasDest) {
            out << "    " << regName(ins.dest.id) << " = " << operandExpr(ins.args[0]) << ";\n";
            state.nativeObjects[ins.dest.id.value] =
                RegCNativeObjectState{objectState->kind, regName(ins.dest.id)};
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
        out << "    " << cType(ins.dest.type) << " string_value_" << valueTemp << " = "
            << defaultValue(ins.dest.type) << ";\n";
        std::ostringstream call;
        call << decision.symbol << "(_mj2x_regc_current_context()";
        for (const auto& arg : ins.args) call << ", " << operandExpr(arg);
        call << ", &string_value_" << valueTemp << ")";
        emitRegCStatusCheck(out, state, call.str(), "String intrinsic");
        out << "    " << regName(ins.dest.id) << " = string_value_" << valueTemp << ";\n";
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
        out << "    ";
        if (ins.hasDest) out << regName(ins.dest.id) << " = ";
        out << decision.symbol << "(";
        for (size_t i = 0; i < ins.args.size(); ++i) {
            if (i) out << ", ";
            out << operandExpr(ins.args[i]);
        }
        out << ");\n";
        return decision;
    }

    out << "    ";
    if (ins.hasDest) out << regName(ins.dest.id) << " = ";
    out << decision.symbol << "(";
    bool needComma = false;
    if (decision.passContext) {
        out << "_mj2x_regc_current_context()";
        needComma = true;
    }
    for (const auto& arg : ins.args) {
        if (needComma) out << ", ";
        out << operandExpr(arg);
        needComma = true;
    }
    out << ");\n";
    return decision;
}

bool emitEdgeCopies(std::ostringstream& out, const std::vector<RegCopy>& copies,
                    const RegCFunctionEmitState& state, int& edgeTempIndex, std::string& reason)
{
    if (copies.empty()) return true;

    // Edge copies are parallel phi transfers.  Stage sources in temporaries so
    // cycles such as r1<-r2 and r2<-r1 remain well-defined for later RegC work.
    std::vector<int> tempIds;
    tempIds.reserve(copies.size());
    for (size_t i = 0; i < copies.size(); ++i) {
        const RegCNativeObjectState* objectState = nativeObjectFor(state, copies[i].source);
        if (objectState != nullptr &&
            objectState->kind != RegCIntrinsicNativeObjectKind::StringBuilder) {
            reason = "native object escaped through CFG edge copy";
            return false;
        }
        const int tempId = edgeTempIndex++;
        tempIds.push_back(tempId);
        out << "    edge_tmp_" << tempId << " = (" << cType(copies[i].dest.type) << ")("
            << operandExpr(copies[i].source) << ");\n";
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        out << "    " << regName(copies[i].dest.id) << " = edge_tmp_" << tempIds[i] << ";\n";
    }
    return true;
}

bool protectsBlock(const RegExceptionRegion& region, const RegBlock* block)
{
    return std::find(region.protectedBlocks.begin(), region.protectedBlocks.end(), block) !=
           region.protectedBlocks.end();
}

std::vector<const RegExceptionRegion*> activeExceptionRegions(const RegFunction& fn,
                                                              const RegBlock*    block)
{
    std::vector<const RegExceptionRegion*> active;
    for (const auto& region : fn.exceptionRegions) {
        if (protectsBlock(region, block)) active.push_back(&region);
    }
    return active;
}

void emitDefaultReturnAfterException(std::ostringstream& out, SSAType returnType)
{
    if (returnType == SSAType::VOID) {
        out << "        goto _mj2x_regc_cleanup;\n";
    }
    else {
        out << "        _mj2x_regc_result = " << defaultValue(returnType) << ";\n";
        out << "        goto _mj2x_regc_cleanup;\n";
    }
}

void emitExceptionDispatch(std::ostringstream&                           out,
                           const std::vector<const RegExceptionRegion*>& regions,
                           SSAType returnType, int& dispatchIndex)
{
    const int index = dispatchIndex++;
    out << "    if (_mj2x_regc_exception_check()) {\n";
    if (regions.empty()) {
        emitDefaultReturnAfterException(out, returnType);
        out << "    }\n";
        return;
    }
    out << "        mj2x_ref ex_" << index << " = _mj2x_regc_exception_occurred();\n";
    out << "        _mj2x_regc_exception_clear();\n";
    // Match in exception-table order.  A matched handler receives the caught
    // object through its synthetic exception register before the block jump.
    for (const auto* region : regions) {
        out << "        if (_mj2x_regc_exception_matches(ex_" << index << ", "
            << cStringLiteral(region->caughtType) << ")) {\n";
        out << "            " << regName(region->exceptionValue.id) << " = ex_" << index << ";\n";
        out << "            goto block_b" << region->handler->id.value << ";\n";
        out << "        }\n";
    }
    // Preserve Java propagation when no handler accepts the pending exception.
    out << "        _mj2x_regc_throw(ex_" << index << ");\n";
    emitDefaultReturnAfterException(out, returnType);
    out << "    }\n";
}

bool emitInstruction(std::ostringstream& out, const RegInstruction& ins,
                     RegCFunctionEmitState& state, std::string& reason)
{
    if (!isSupportedInstruction(ins, reason)) return false;

    switch (ins.kind) {
    case RegInstructionKind::Copy:
        if (const auto* objectState = nativeObjectFor(state, ins.copy.source)) {
            if (objectState->kind == RegCIntrinsicNativeObjectKind::StringBuilder) {
                out << "    " << regName(ins.copy.dest.id) << " = " << operandExpr(ins.copy.source)
                    << ";\n";
                state.nativeObjects[ins.copy.dest.id.value] = RegCNativeObjectState{
                    objectState->kind, regName(ins.copy.dest.id)};
            }
            else {
                out << "    " << nativeObjectType(objectState->kind) << " "
                    << nativeObjectStorageName(ins.copy.dest.id) << " = "
                    << objectState->storageName << ";\n";
                out << "    " << regName(ins.copy.dest.id) << " = NULL;\n";
                state.nativeObjects[ins.copy.dest.id.value] = RegCNativeObjectState{
                    objectState->kind, nativeObjectStorageName(ins.copy.dest.id)};
            }
        }
        else {
            state.nativeObjects.erase(ins.copy.dest.id.value);
            out << "    " << regName(ins.copy.dest.id) << " = (" << cType(ins.copy.dest.type)
                << ")(" << operandExpr(ins.copy.source) << ");\n";
        }
        return true;
    case RegInstructionKind::Unary:
        out << "    " << regName(ins.dest.id) << " = (" << cType(ins.dest.type) << ")(-"
            << operandExpr(ins.value) << ");\n";
        return true;
    case RegInstructionKind::Binary:
        out << "    " << regName(ins.dest.id) << " = " << binaryExpr(ins) << ";\n";
        return true;
    case RegInstructionKind::Compare:
        out << "    " << regName(ins.dest.id) << " = (" << operandExpr(ins.lhs) << " "
            << compareOp(ins.comparePredicate) << " " << operandExpr(ins.rhs) << ");\n";
        return true;
    case RegInstructionKind::Convert:
        if (!rejectNativeObjectEscape(state, ins.value, "reference conversion", reason))
            return false;
        if (ins.convertKind == SSAConvertKind::ReferenceCast) {
            const std::string cacheName = addClassCache(state, ins.referenceTypeDesc);
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_checkcast_cached("
                << operandExpr(ins.value) << ", &" << cacheName << ");\n";
        }
        else {
            out << "    " << regName(ins.dest.id) << " = (" << cType(ins.targetType) << ")("
                << operandExpr(ins.value) << ");\n";
        }
        return true;
    case RegInstructionKind::LoadField:
        if (!rejectNativeObjectEscape(state, ins.receiver, "field load receiver", reason))
            return false;
        if (ins.field.isStatic) {
            const std::string cacheName = addStaticFieldCache(state, ins);
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_load_cached_"
                << fieldHelperSuffix(ins.field.desc, ins.dest.type) << "_field(&" << cacheName
                << ");\n";
        }
        else {
            const std::string cacheName = addInstanceFieldCache(state, ins);
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_load_";
            if (!cacheName.empty()) {
                out << "cached_instance_" << fieldHelperSuffix(ins.field.desc, ins.dest.type)
                    << "_field(&" << cacheName << ", " << operandExpr(ins.receiver) << ");\n";
            }
            else {
                out << fieldHelperSuffix(ins.field.desc, ins.dest.type) << "_field(0, "
                    << operandExpr(ins.receiver) << ", " << cStringLiteral(ins.field.owner)
                    << ", " << cStringLiteral(ins.field.name) << ", "
                    << cStringLiteral(ins.field.desc) << ");\n";
            }
        }
        return true;
    case RegInstructionKind::StoreField:
        if (!rejectNativeObjectEscape(state, ins.receiver, "field store receiver", reason))
            return false;
        {
        std::string valueExpression;
        const bool materialized =
            emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
        if (!materialized &&
            !rejectNativeObjectEscape(state, ins.value, "field store value", reason)) {
            return false;
        }
        if (materialized) out << "    if (!_mj2x_regc_exception_check()) {\n";
        if (ins.field.isStatic) {
            const std::string cacheName = addStaticFieldCache(state, ins);
            out << "    _mj2x_regc_store_cached_"
                << fieldHelperSuffix(ins.field.desc, ins.value.type) << "_field(&" << cacheName
                << ", " << valueExpression << ");\n";
        }
        else {
            const std::string cacheName = addInstanceFieldCache(state, ins);
            out << "    _mj2x_regc_store_";
            if (!cacheName.empty()) {
                out << "cached_instance_" << fieldHelperSuffix(ins.field.desc, ins.value.type)
                    << "_field(&" << cacheName << ", " << operandExpr(ins.receiver) << ", "
                    << valueExpression << ");\n";
            }
            else {
                out << fieldHelperSuffix(ins.field.desc, ins.value.type) << "_field(0, "
                    << operandExpr(ins.receiver) << ", " << cStringLiteral(ins.field.owner)
                    << ", " << cStringLiteral(ins.field.name) << ", "
                    << cStringLiteral(ins.field.desc) << ", " << valueExpression
                    << ");\n";
            }
        }
        if (materialized) out << "    }\n";
        return true;
        }
    case RegInstructionKind::ArrayLoad:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array load receiver", reason))
            return false;
        out << "    " << regName(ins.dest.id) << " = _mj2x_regc_load_"
            << arrayHelperSuffix(ins.elementType) << "_array(" << operandExpr(ins.receiver) << ", "
            << operandExpr(ins.index) << ");\n";
        return true;
    case RegInstructionKind::ArrayStore:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array store receiver", reason))
            return false;
        {
        std::string valueExpression;
        const bool materialized =
            emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
        if (!materialized &&
            !rejectNativeObjectEscape(state, ins.value, "array store value", reason)) {
            return false;
        }
        if (materialized) out << "    if (!_mj2x_regc_exception_check()) {\n";
        out << "    _mj2x_regc_store_" << arrayHelperSuffix(ins.elementType) << "_array("
            << operandExpr(ins.receiver) << ", " << operandExpr(ins.index) << ", "
            << valueExpression << ");\n";
        if (materialized) out << "    }\n";
        return true;
        }
    case RegInstructionKind::Call:
    {
        // JDK intrinsic recognition goes through the registry so fallback
        // and unsupported reasons remain explicit as coverage grows.
        {
            RegCIntrinsicDecision decision = emitIntrinsicCall(out, ins, state);
            if (decision.kind == RegCIntrinsicDecisionKind::Emit) return true;
            if (decision.kind == RegCIntrinsicDecisionKind::Unsupported) {
                reason = decision.reason;
                return false;
            }
        }
        std::vector<std::string> argumentExpressions;
        argumentExpressions.reserve(ins.args.size());
        bool hasMaterializedArgument = false;
        for (const auto& arg : ins.args) {
            std::string expression;
            const bool materialized =
                emitMaterializedStringBuilder(out, state, arg, expression);
            if (!materialized && nativeObjectFor(state, arg) != nullptr) {
                reason = "native object escaped through generic call " + ins.method.owner + "." +
                         ins.method.name;
                return false;
            }
            hasMaterializedArgument = hasMaterializedArgument || materialized;
            argumentExpressions.push_back(std::move(expression));
        }
        if (hasMaterializedArgument) out << "    if (!_mj2x_regc_exception_check()) {\n";
        const bool        staticCall = isStaticCall(ins);
        const bool        selfCall   = isSelfStaticCall(state, ins);
        const std::string cacheName =
            (staticCall && !selfCall) ? addStaticMethodCache(state, ins) : "";
        const std::string instanceCacheName =
            (!staticCall && !selfCall) ? addInstanceMethodCache(state, ins) : "";
        if (ins.hasDest) {
            out << "    " << regName(ins.dest.id) << " = ";
            if (selfCall) {
                out << state.functionName << "("
                    << directCallArgs(ins.args, argumentExpressions) << ");\n";
            }
            else {
                out << "_mj2x_regc_call_";
                if (staticCall) {
                    out << "cached_" << helperSuffix(ins.dest.type) << "(&" << cacheName << ", "
                        << callArgs(ins.args, argumentExpressions) << ");\n";
                }
                else if (!instanceCacheName.empty()) {
                    out << "cached_instance_" << helperSuffix(ins.dest.type) << "(&"
                        << instanceCacheName << ", "
                        << callArgs(ins.args, argumentExpressions) << ");\n";
                }
                else {
                    out << helperSuffix(ins.dest.type) << "(" << cStringLiteral(ins.method.owner)
                        << ", " << cStringLiteral(ins.method.name) << ", "
                        << cStringLiteral(ins.method.desc) << ", 0, "
                        << callArgs(ins.args, argumentExpressions)
                        << ");\n";
                }
            }
        }
        else {
            out << "    ";
            if (selfCall) {
                out << state.functionName << "("
                    << directCallArgs(ins.args, argumentExpressions) << ");\n";
            }
            else {
                out << "_mj2x_regc_call_";
                if (staticCall) {
                    out << "cached_void(&" << cacheName << ", "
                        << callArgs(ins.args, argumentExpressions) << ");\n";
                }
                else if (!instanceCacheName.empty()) {
                    out << "cached_instance_void(&" << instanceCacheName << ", "
                        << callArgs(ins.args, argumentExpressions) << ");\n";
                }
                else {
                    out << "void(" << cStringLiteral(ins.method.owner) << ", "
                        << cStringLiteral(ins.method.name) << ", "
                        << cStringLiteral(ins.method.desc) << ", 0, "
                        << callArgs(ins.args, argumentExpressions)
                        << ");\n";
                }
            }
        }
        if (hasMaterializedArgument) out << "    }\n";
        return true;
    }
    case RegInstructionKind::Allocate:
        if (ins.allocationKind == SSAAllocationKind::Object) {
            if (ins.typeDesc == "java/util/Random") {
                out << "    mj2x_random " << nativeObjectStorageName(ins.dest.id) << " = {0};\n";
                out << "    " << regName(ins.dest.id) << " = NULL;\n";
                state.nativeObjects[ins.dest.id.value] = RegCNativeObjectState{
                    RegCIntrinsicNativeObjectKind::Random, nativeObjectStorageName(ins.dest.id)};
            }
            else if (ins.typeDesc == "java/lang/StringBuilder" &&
                     state.nativeStringBuilderRegisters.count(ins.dest.id.value) != 0) {
                out << "    " << regName(ins.dest.id) << " = (void*)&"
                    << stringBuilderOwnerName(ins.dest.id) << ";\n";
                state.nativeObjects[ins.dest.id.value] = RegCNativeObjectState{
                    RegCIntrinsicNativeObjectKind::StringBuilder,
                    regName(ins.dest.id)};
            }
            else {
                out << "    " << regName(ins.dest.id) << " = ";
                if (state.cachePool != nullptr) {
                    const std::string cacheName = addClassCache(state, ins.typeDesc);
                    out << "_mj2x_regc_alloc_object_cached(&" << cacheName << ");\n";
                }
                else {
                    out << "_mj2x_regc_alloc_object(" << cStringLiteral(ins.typeDesc) << ");\n";
                }
            }
        }
        else if (ins.allocationKind == SSAAllocationKind::Array) {
            const RegOperand length =
                ins.args.empty() ? RegOperand::fromInt(0, SSAType::I32) : ins.args[0];
            if (state.cachePool != nullptr && isReferenceArrayDesc(ins.typeDesc)) {
                const std::string cacheName = addClassCache(state, arrayComponentDesc(ins.typeDesc));
                out << "    " << regName(ins.dest.id)
                    << " = _mj2x_regc_alloc_ref_array_cached(&" << cacheName << ", "
                    << operandExpr(length) << ");\n";
            }
            else {
                out << "    " << regName(ins.dest.id) << " = _mj2x_regc_alloc_array("
                    << cStringLiteral(ins.typeDesc) << ", " << operandExpr(length) << ");\n";
            }
        }
        else {
            out << "    {\n";
            out << "        int32_t dims[" << ins.args.size() << "];\n";
            for (size_t i = 0; i < ins.args.size(); ++i) {
                out << "        dims[" << i << "] = " << operandExpr(ins.args[i]) << ";\n";
            }
            const std::vector<std::string> componentDescs =
                state.cachePool != nullptr ? multiArrayComponentDescs(ins.typeDesc, ins.args.size())
                                           : std::vector<std::string>{};
            if (!componentDescs.empty()) {
                out << "        mj2x_regc_class_cache* component_caches[" << componentDescs.size()
                    << "] = {";
                for (size_t i = 0; i < componentDescs.size(); ++i) {
                    if (i != 0) out << ", ";
                    const std::string cacheName = addClassCache(state, componentDescs[i]);
                    out << "&" << cacheName;
                }
                out << "};\n";
                out << "        " << regName(ins.dest.id)
                    << " = _mj2x_regc_alloc_multi_array_cached("
                    << cStringLiteral(ins.typeDesc) << ", " << ins.args.size()
                    << ", dims, component_caches);\n";
            }
            else {
                out << "        " << regName(ins.dest.id) << " = _mj2x_regc_alloc_multi_array("
                    << cStringLiteral(ins.typeDesc) << ", " << ins.args.size() << ", dims);\n";
            }
            out << "    }\n";
        }
        return true;
    case RegInstructionKind::Intrinsic:
        if (ins.intrinsicName == "array.length") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "array.length",
                                          reason))
                return false;
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_array_length("
                << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
            return true;
        }
        if (ins.intrinsicName.rfind("instanceof:", 0) == 0) {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "instanceof",
                                          reason))
                return false;
            const std::string cacheName = addClassCache(state, ins.intrinsicName.substr(11));
            out << "    " << regName(ins.dest.id) << " = _mj2x_regc_instanceof_cached("
                << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ", &" << cacheName
                << ");\n";
            return true;
        }
        if (ins.intrinsicName == "monitor.enter") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.enter",
                                          reason))
                return false;
            out << "    _mj2x_regc_monitor_enter("
                << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
            return true;
        }
        if (ins.intrinsicName == "monitor.exit") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.exit",
                                          reason))
                return false;
            out << "    _mj2x_regc_monitor_exit("
                << (ins.args.empty() ? "NULL" : operandExpr(ins.args[0])) << ");\n";
            return true;
        }
        reason = "unsupported RegC intrinsic: " + ins.intrinsicName;
        return false;
    case RegInstructionKind::Barrier:
        out << "    mj2x_regc_barrier(_mj2x_regc_current_context(), "
            << cStringLiteral(ins.barrierReason) << ");\n";
        return true;
    default: break;
    }

    reason = "unreachable RegC instruction path";
    return false;
}

bool emitTerminator(std::ostringstream& out, const RegTerminator& terminator, SSAType returnType,
                    const std::vector<const RegExceptionRegion*>& activeRegions,
                    RegCFunctionEmitState& state, int& dispatchIndex, int& edgeTempIndex,
                    std::string& reason)
{
    return std::visit(
        [&](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                out << "    {\n";
                if (!emitEdgeCopies(out, t.target.copies, state, edgeTempIndex, reason))
                    return false;
                out << "    goto block_b" << t.target.block->id.value << ";\n";
                out << "    }\n";
                return true;
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.condition, "branch condition", reason))
                    return false;
                out << "    if (" << operandExpr(t.condition) << ") {\n";
                if (!emitEdgeCopies(out, t.trueTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out << "        goto block_b" << t.trueTarget.block->id.value << ";\n";
                out << "    } else {\n";
                if (!emitEdgeCopies(out, t.falseTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out << "        goto block_b" << t.falseTarget.block->id.value << ";\n";
                out << "    }\n";
                return true;
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "switch value", reason)) return false;
                out << "    switch (" << operandExpr(t.value) << ") {\n";
                for (const auto& c : t.cases) {
                    out << "    case " << c.key << ": {\n";
                    if (!emitEdgeCopies(out, c.target.copies, state, edgeTempIndex, reason))
                        return false;
                    out << "        goto block_b" << c.target.block->id.value << ";\n";
                    out << "    }\n";
                }
                out << "    default: {\n";
                if (!emitEdgeCopies(out, t.defaultTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out << "        goto block_b" << t.defaultTarget.block->id.value << ";\n";
                out << "    }\n";
                out << "    }\n";
                return true;
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (t.hasValue) {
                    const RegCNativeObjectState* objectState = nativeObjectFor(state, t.value);
                    if (objectState != nullptr &&
                        objectState->kind == RegCIntrinsicNativeObjectKind::StringBuilder) {
                        std::string materializedExpression;
                        emitMaterializedStringBuilder(
                            out, state, t.value, materializedExpression);
                        emitExceptionDispatch(out, activeRegions, returnType, dispatchIndex);
                        out << "    _mj2x_regc_result = " << materializedExpression << ";\n";
                    }
                    else {
                        if (objectState != nullptr) {
                            reason = "native object escaped through method return";
                            return false;
                        }
                        out << "    _mj2x_regc_result = " << operandExpr(t.value) << ";\n";
                    }
                }
                out << "    goto _mj2x_regc_cleanup;\n";
                return true;
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "throw", reason)) return false;
                out << "    _mj2x_regc_throw(" << operandExpr(t.value) << ");\n";
                emitExceptionDispatch(out, activeRegions, returnType, dispatchIndex);
                if (returnType != SSAType::VOID)
                    out << "    _mj2x_regc_result = " << defaultValue(returnType) << ";\n";
                out << "    goto _mj2x_regc_cleanup;\n";
                return true;
            }
            else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                if (returnType != SSAType::VOID)
                    out << "    _mj2x_regc_result = " << defaultValue(returnType) << ";\n";
                out << "    goto _mj2x_regc_cleanup;\n";
                return true;
            }
            else {
                reason = "unknown RegIR terminator";
                return false;
            }
        },
        terminator);
}

}   // namespace

namespace {

std::string poolCacheKey(const std::string& first, const std::string& second,
                         const std::string& third)
{
    return first + "\n" + second + "\n" + third;
}

}   // namespace

std::string RegCCachePool::staticMethodCache(const std::string& owner, const std::string& name,
                                             const std::string& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticMethodCacheNames_.find(key);
    if (it != staticMethodCacheNames_.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_cache_tu_" +
                                  std::to_string(staticMethodCacheIndex_++);
    std::ostringstream decl;
    decl << "static mj2x_regc_static_method_cache " << cacheName << " = {"
         << codegen::cStringLiteral(owner) << ", " << codegen::cStringLiteral(name) << ", "
         << codegen::cStringLiteral(desc) << ", NULL, NULL};\n";
    declarations_.push_back(decl.str());
    staticMethodCacheNames_.emplace(key, cacheName);
    return cacheName;
}

std::string RegCCachePool::instanceMethodCache(const std::string& owner, const std::string& name,
                                               const std::string& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceMethodCacheNames_.find(key);
    if (it != instanceMethodCacheNames_.end()) return it->second;

    const std::string cacheName = "_mj2x_regc_instance_cache_tu_" +
                                  std::to_string(instanceMethodCacheIndex_++);
    std::ostringstream decl;
    decl << "static mj2x_regc_instance_method_cache " << cacheName << " = {"
         << codegen::cStringLiteral(owner) << ", " << codegen::cStringLiteral(name) << ", "
         << codegen::cStringLiteral(desc) << ", NULL, NULL};\n";
    declarations_.push_back(decl.str());
    instanceMethodCacheNames_.emplace(key, cacheName);
    return cacheName;
}

std::string RegCCachePool::staticFieldCache(const std::string& owner, const std::string& name,
                                            const std::string& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticFieldCacheNames_.find(key);
    if (it != staticFieldCacheNames_.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_field_cache_tu_" +
                                  std::to_string(staticFieldCacheIndex_++);
    std::ostringstream decl;
    decl << "static mj2x_regc_static_field_cache " << cacheName << " = {"
         << codegen::cStringLiteral(owner) << ", " << codegen::cStringLiteral(name) << ", "
         << codegen::cStringLiteral(desc) << ", NULL, NULL};\n";
    declarations_.push_back(decl.str());
    staticFieldCacheNames_.emplace(key, cacheName);
    return cacheName;
}

std::string RegCCachePool::instanceFieldCache(const std::string& owner, const std::string& name,
                                              const std::string& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceFieldCacheNames_.find(key);
    if (it != instanceFieldCacheNames_.end()) return it->second;

    const std::string cacheName = "_mj2x_regc_instance_field_cache_tu_" +
                                  std::to_string(instanceFieldCacheIndex_++);
    std::ostringstream decl;
    decl << "static mj2x_regc_instance_field_cache " << cacheName << " = {"
         << codegen::cStringLiteral(owner) << ", " << codegen::cStringLiteral(name) << ", "
         << codegen::cStringLiteral(desc) << ", NULL, NULL};\n";
    declarations_.push_back(decl.str());
    instanceFieldCacheNames_.emplace(key, cacheName);
    return cacheName;
}

std::string RegCCachePool::classCache(const std::string& desc)
{
    auto it = classCacheNames_.find(desc);
    if (it != classCacheNames_.end()) return it->second;

    const std::string  cacheName = "_mj2x_regc_class_cache_tu_" +
                                  std::to_string(classCacheIndex_++);
    std::ostringstream decl;
    decl << "static mj2x_regc_class_cache " << cacheName << " = {"
         << codegen::cStringLiteral(desc) << ", NULL};\n";
    declarations_.push_back(decl.str());
    classCacheNames_.emplace(desc, cacheName);
    return cacheName;
}

std::string RegCCachePool::renderDeclarations() const
{
    std::ostringstream out;
    for (const auto& declaration : declarations_) {
        out << declaration;
    }
    if (!declarations_.empty()) out << "\n";
    return out.str();
}

bool RegCCachePool::empty() const
{
    return declarations_.empty();
}

void emitRegCHelperPrelude(std::ostream& out)
{
    emitHelperPrelude(out);
}

RegCEmitter::RegCEmitter(RegCCachePool* cachePool)
    : cachePool_(cachePool)
{}

RegCEmitResult RegCEmitter::emitFunction(const RegFunction& fn) const
{
    RegCEmitResult result;
    result.functionName = functionName(fn.symbol);
    if (!fn.entry) {
        result.reason = "RegC requires an entry block";
        return result;
    }

    std::string validationReason;
    if (!validateRegIRNoStackSlots(fn, validationReason)) {
        result.reason = validationReason;
        return result;
    }

    std::map<uint32_t, SSAType> regs;
    if (!collectRegisters(fn, regs, result.reason)) return result;
    const std::vector<SSAType> edgeTempTypes = collectEdgeCopyTempTypes(fn);
    const RegCStringBuilderPlan stringBuilderPlan = analyzeStringBuilders(fn);
    const std::set<uint32_t> nativeWrapperRegisters = analyzeNativeWrappers(fn);

    RegCFunctionEmitState emitState;
    emitState.functionName           = result.functionName;
    emitState.symbol                 = fn.symbol;
    emitState.cachePool              = cachePool_;
    emitState.nativeStringBuilderRegisters = stringBuilderPlan.nativeRegisters;
    emitState.nativeStringBuilderOwners    = stringBuilderPlan.ownerRegisters;
    emitState.nativeStringBuilderOwnerByRegister = stringBuilderPlan.ownerByRegister;
    emitState.nativeWrapperRegisters = nativeWrapperRegisters;
    for (uint32_t id : stringBuilderPlan.nativeRegisters) {
        emitState.nativeObjects.emplace(
            id,
            RegCNativeObjectState{RegCIntrinsicNativeObjectKind::StringBuilder,
                                  regName(RegId{id})});
    }
    int                dispatchIndex = 0;
    int                edgeTempIndex = 0;
    std::ostringstream body;
    for (const auto& block : fn.blocks) {
        auto activeRegions = activeExceptionRegions(fn, block.get());
        body << "block_b" << block->id.value << ":\n";
        for (const auto& ins : block->instructions) {
            if (!emitInstruction(body, ins, emitState, result.reason)) return result;
            if (ins.effect.mayThrow) {
                emitExceptionDispatch(body, activeRegions, fn.symbol.returnType, dispatchIndex);
            }
        }
        if (!emitTerminator(body,
                            block->terminator,
                            fn.symbol.returnType,
                            activeRegions,
                            emitState,
                            dispatchIndex,
                            edgeTempIndex,
                            result.reason)) {
            return result;
        }
        body << "\n";
    }

    std::ostringstream out;
    out << "/* Generated by mj2x RegC MVP */\n";
    for (const auto& declaration : emitState.staticMethodCacheDeclarations) {
        out << declaration;
    }
    for (const auto& declaration : emitState.staticFieldCacheDeclarations) {
        out << declaration;
    }
    for (const auto& declaration : emitState.classCacheDeclarations) {
        out << declaration;
    }
    if (!emitState.staticMethodCacheDeclarations.empty() ||
        !emitState.staticFieldCacheDeclarations.empty() || !emitState.classCacheDeclarations.empty())
        out << "\n";

    out << "static " << cType(fn.symbol.returnType) << " " << result.functionName << "(";
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out << ", ";
        out << cType(fn.parameters[i].type) << " " << regName(fn.parameters[i].id);
    }
    out << ") {\n";

    if (fn.symbol.returnType != SSAType::VOID) {
        out << "    " << cType(fn.symbol.returnType) << " _mj2x_regc_result = "
            << defaultValue(fn.symbol.returnType) << ";\n";
    }

    for (const auto& [id, type] : regs) {
        bool isParameter = false;
        for (const auto& param : fn.parameters) {
            if (param.id.value == id) {
                isParameter = true;
                break;
            }
        }
        if (!isParameter) {
            out << "    " << cType(type) << " " << regName(RegId{id}) << " = 0;\n";
        }
    }
    for (size_t i = 0; i < edgeTempTypes.size(); ++i) {
        out << "    " << cType(edgeTempTypes[i]) << " edge_tmp_" << i << " = 0;\n";
    }
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        out << "    mj2x_string_builder* " << stringBuilderOwnerName(RegId{owner})
            << " = NULL;\n";
        out << "    mj2x_ref " << stringBuilderMaterializedName(RegId{owner}) << " = NULL;\n";
    }
    if (!regs.empty()) out << "\n";
    out << "    goto block_b" << fn.entry->id.value << ";\n\n";
    out << body.str();

    out << "_mj2x_regc_cleanup:\n";
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        out << "    mj2x_string_builder_destroy(" << stringBuilderOwnerName(RegId{owner})
            << ");\n";
    }
    if (fn.symbol.returnType == SSAType::VOID)
        out << "    return;\n";
    else
        out << "    return _mj2x_regc_result;\n";

    out << "}\n";

    result.source = out.str();
    if (!validateRegCNoStackAnchors(result.source, result.reason)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.reason.clear();
    return result;
}

bool validateRegCNoStackAnchors(const std::string& source, std::string& reason)
{
    const char* anchors[] = {
        "cstack[",
        "cstack",
        "stack[",
        "union jvalue",
        " sp ",
        "sp--",
        "sp++",
        "Cannot invoke",
        "cclasses[",
        "cmethods[",
    };
    for (const char* anchor : anchors) {
        if (source.find(anchor) != std::string::npos) {
            reason = std::string("RegC output contains stack-machine anchor: ") + anchor;
            return false;
        }
    }
    reason.clear();
    return true;
}

}   // namespace mj2x

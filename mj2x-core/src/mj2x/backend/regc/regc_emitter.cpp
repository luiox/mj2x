#include "regc_emitter.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;

/// Emit one invokedynamic site call.  Defined after emitFunction in this file.
bool emitInvokeDynamic(codegen::CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       std::string& reason);

namespace {

// cStringLiteral is shared with StackC via codegen::cStringLiteral; alias it
// here so the existing call sites stay one-line.
using codegen::cStringLiteral;
// 源码拼接统一走 CSourceStream（Utf8 家族三层模型的排版层）：局部片段构建器与
// emit 路径的 sink 都用它，其 append 重载覆盖 Utf8String/Utf8StringRef/字面量/
// string_view，消除了 Utf8StringBuilder 无法直接接 std::string 的缺口。
using codegen::CSourceStream;

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

/// 解析方法描述符 "(...)" 中的参数类型标签：基础类型为 Z B C S I J F D，
/// 引用类型记 L，数组记 [。顺序即调用参数顺序（实例调用不含 receiver）。
std::vector<char> descriptorParamTags(const ca::str::Utf8StringRef& desc)
{
    std::vector<char> tags;
    const ca::u8*     p = desc.data();
    if (p == nullptr || *p != '(') return tags;
    ++p;
    while (*p != ')' && *p != '\0') {
        if (*p == '[') {
            tags.push_back(static_cast<char>('['));
            while (*p == '[') ++p;
            if (*p == 'L') {
                while (*p != ';' && *p != '\0') ++p;
            }
        }
        else if (*p == 'L') {
            tags.push_back(static_cast<char>('L'));
            while (*p != ';' && *p != '\0') ++p;
        }
        else {
            tags.push_back(static_cast<char>(*p));
        }
        ++p;
    }
    return tags;
}

/// 描述符返回类型标签（')' 之后的首字符），无效描述符返回 '\0'。
char descriptorReturnTag(const ca::str::Utf8StringRef& desc)
{
    const ca::u8* p = desc.data();
    if (p == nullptr) return '\0';
    while (*p != ')' && *p != '\0') ++p;
    if (*p != ')') return '\0';
    return static_cast<char>(p[1]);
}

/// JNI CallXxxMethodA 的类型族名，如 'I' -> "Int"。
const char* jniCallFamily(char tag)
{
    switch (tag) {
    case 'Z': return "Boolean";
    case 'B': return "Byte";
    case 'C': return "Char";
    case 'S': return "Short";
    case 'I': return "Int";
    case 'J': return "Long";
    case 'F': return "Float";
    case 'D': return "Double";
    case 'V': return "Void";
    default: return "Object";
    }
}

/// jvalue 联合体成员名，如 'I' -> "i"。
const char* jvalueSlotName(char tag)
{
    switch (tag) {
    case 'Z': return "z";
    case 'B': return "b";
    case 'C': return "c";
    case 'S': return "s";
    case 'I': return "i";
    case 'J': return "j";
    case 'F': return "f";
    case 'D': return "d";
    default: return "l";
    }
}

/// 值写入 jvalue 槽前的显式强转，如 'I' -> "(jint)"。
const char* jniValueCast(char tag)
{
    switch (tag) {
    case 'Z': return "(jboolean)";
    case 'B': return "(jbyte)";
    case 'C': return "(jchar)";
    case 'S': return "(jshort)";
    case 'I': return "(jint)";
    case 'J': return "(jlong)";
    case 'F': return "(jfloat)";
    case 'D': return "(jdouble)";
    default: return "(jobject)";
    }
}

/// JNI 调用结果到寄存器 C 类型的强转；引用结果转 mj2x_ref。
ca::str::Utf8String jniResultCast(char returnTag, SSAType destType)
{
    CSourceStream os;
    os.append("(");
    os.append((returnTag == 'L' || returnTag == '[') ? "mj2x_ref" : cType(destType));
    os.append(")");
    return os.build();
}

ca::str::Utf8String floatingOperandExpr(double value, SSAType type)
{
    CSourceStream os;
    if (type == SSAType::F32) {
        const float floatValue = static_cast<float>(value);
        uint32_t    bits       = 0;
        std::memcpy(&bits, &floatValue, sizeof(bits));
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08x", bits);
        os.append("_mj2x_regc_f32_from_bits((uint32_t)0x");
        os.append(buffer);
        os.append("u)");
        return os.build();
    }

    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char buffer[20];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(bits));
    os.append("_mj2x_regc_f64_from_bits((uint64_t)0x");
    os.append(buffer);
    os.append("ULL)");
    return os.build();
}

ca::str::Utf8String operandExpr(const RegOperand& operand)
{
    switch (operand.kind) {
    case RegOperandKind::Register:
    {
        const std::string name = regName(operand.reg);
        return ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(name.data()), name.size());
    }
    case RegOperandKind::IntLiteral:
    {
        CSourceStream os;
        if (operand.type == SSAType::I64) {
            os.append("(int64_t)");
            os.append(std::to_string(operand.intValue));
            os.append("LL");
        }
        else {
            os.append(std::to_string(operand.intValue));
        }
        return os.build();
    }
    case RegOperandKind::FloatLiteral: return floatingOperandExpr(operand.floatValue, operand.type);
    case RegOperandKind::NullLiteral: return ca::str::Utf8String("NULL");
    case RegOperandKind::Opaque:
        // A String LDC carries its UTF-8 content after "ldc_string:" so the
        // String intrinsic path can materialize the jstring without going
        // through JNI call dispatch.  Other opaques stay as annotated NULL.
        if (operand.tag.rfind("ldc_string:", 0) == 0) {
            CSourceStream os;
            os.append("_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), ");
            os.append(cStringLiteral(operand.tag.substr(std::string("ldc_string:").size())));
            os.append(")");
            return os.build();
        }
        // LDC Class 常量（Foo.class）携带 internal name 在 "ldc_class:" 之后，
        // 发射 FindClass 物质化为 jclass。
        if (operand.tag.rfind("ldc_class:", 0) == 0) {
            CSourceStream os;
            os.append("_mj2x_regc_class_literal(_mj2x_regc_current_context(), ");
            os.append(cStringLiteral(operand.tag.substr(std::string("ldc_class:").size())));
            os.append(")");
            return os.build();
        }
        CSourceStream os;
        os.append("NULL /* opaque:");
        os.append(operand.tag.data(), operand.tag.size());
        os.append(" */");
        return os.build();
    }
    return ca::str::Utf8String("0");
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

const char* fieldHelperSuffix(const ca::str::Utf8StringRef& desc, SSAType fallback)
{
    // JVM narrow fields are int-like on the operand stack but require their
    // descriptor-specific JNI accessors at the native boundary.
    if (!desc.is_empty()) {
        switch (desc.data()[0]) {
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

bool isReferenceArrayDesc(const ca::str::Utf8StringRef& desc)
{
    std::string_view sv = desc;
    return sv.size() > 1 && sv[0] == '[' && (sv[1] == 'L' || sv[1] == '[');
}

std::string arrayComponentDesc(const ca::str::Utf8StringRef& desc)
{
    std::string_view sv = desc;
    if (sv.size() <= 1) return std::string{sv};
    std::string component{sv.substr(1)};
    if (component.size() >= 2 && component.front() == 'L' && component.back() == ';') {
        return component.substr(1, component.size() - 2);
    }
    return component;
}

std::vector<std::string> multiArrayComponentDescs(const ca::str::Utf8StringRef& desc, size_t dimCount)
{
    std::vector<std::string> components;
    std::string              current = desc.to_std_string();
    for (size_t i = 0; i < dimCount && isReferenceArrayDesc(
               ca::str::Utf8StringRef::from_string_view(current));
         ++i) {
        components.push_back(arrayComponentDesc(ca::str::Utf8StringRef::from_string_view(current)));
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
    for (const auto& region : regions) {
        collectRegister(regs, region.exceptionValue);
        for (const auto& edge : region.edges) collectCopies(regs, edge.copies);
    }
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
    RegCCommentLevel                          commentLevel = RegCCommentLevel::Summary;
    bool                                      traceCalls   = false;
    /// native-to-native 直接调用上下文（compileMethod 传入，nullptr 关闭）。
    CompilerIndex*                            classIndex          = nullptr;
    const std::unordered_map<std::string, std::string>* convertedFunctions = nullptr;
    const std::unordered_set<std::string>*    indyFunctions       = nullptr;
    /// 动态实例调用是否启用 resolved native dispatch（多文件模式关闭：
    /// 函数表只在单文件 TU 内可见）。
    bool                                      resolvedDispatch    = false;
    std::vector<ca::str::Utf8String>         staticMethodCacheDeclarations;
    std::vector<ca::str::Utf8String>       staticFieldCacheDeclarations;
    std::vector<ca::str::Utf8String>       classCacheDeclarations;
    std::map<std::string, ca::str::Utf8String> staticMethodCacheNames;
    std::map<std::string, ca::str::Utf8String> staticFieldCacheNames;
    std::map<std::string, ca::str::Utf8String> classCacheNames;
    int                                       staticMethodCacheIndex = 0;
    int                                       staticFieldCacheIndex  = 0;
    int                                       classCacheIndex        = 0;
    int                                       stringBuilderTempIndex = 0;
    std::set<uint32_t>                        nativeStringBuilderRegisters;
    std::vector<uint32_t>                     nativeStringBuilderOwners;
    std::map<uint32_t, uint32_t>              nativeStringBuilderOwnerByRegister;
    std::set<uint32_t>                        nativeWrapperRegisters;
    /// Edge-copy temporary types actually emitted, indexed by edgeTempIndex.
    /// Captured during emit so the declaration block declares exactly the temporaries
    /// that are used (a block with multiple mayThrow instructions re-emits its
    /// exception edge copies once per throw site, so the count can exceed the static
    /// CFG edge count).
    std::vector<SSAType>                      edgeTempTypes;
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
    case RegCIntrinsicNativeObjectKind::StringBuffer: return "mj2x_string_buffer";
    default: return "mj2x_ref";
    }
}

bool isMutableStringKind(RegCIntrinsicNativeObjectKind kind)
{
    return kind == RegCIntrinsicNativeObjectKind::StringBuilder ||
           kind == RegCIntrinsicNativeObjectKind::StringBuffer;
}

const char* mutableStringDisplayName(RegCIntrinsicNativeObjectKind kind)
{
    return kind == RegCIntrinsicNativeObjectKind::StringBuffer ? "StringBuffer" : "StringBuilder";
}

ca::str::Utf8String stringBuilderOwnerName(RegId id)
{
    CSourceStream os;
    os.append("native_owner_r");
    os.append(std::to_string(id.value));
    return os.build();
}

ca::str::Utf8String stringBuilderMaterializedName(RegId id)
{
    CSourceStream os;
    os.append("native_materialized_r");
    os.append(std::to_string(id.value));
    return os.build();
}

ca::str::Utf8String stringBuilderHandleExpr(const RegOperand&             operand,
                                            RegCIntrinsicNativeObjectKind kind)
{
    CSourceStream os;
    os.append("(");
    os.append(nativeObjectType(kind));
    os.append("**)");
    os.append(operandExpr(operand));
    return os.build();
}

ca::str::Utf8String stringBuilderStateExpr(const RegOperand&             operand,
                                           RegCIntrinsicNativeObjectKind kind)
{
    CSourceStream os;
    os.append("*(");
    os.append(stringBuilderHandleExpr(operand, kind));
    os.append(")");
    return os.build();
}

void emitRegCStatusCheck(CSourceStream& out, RegCFunctionEmitState& state,
                         const ca::str::Utf8StringRef& statusExpr,
                         const std::string&            operation)
{
    const int temp = state.stringBuilderTempIndex++;
    out.append("    mj2x_regc_status runtime_status_");
    out.append(std::to_string(temp));
    out.append(" = ");
    out.append(statusExpr);
    out.append(";\n");
    out.append("    if (runtime_status_");
    out.append(std::to_string(temp));
    out.append(" != MJ2X_REGC_STATUS_OK && runtime_status_");
    out.append(std::to_string(temp));
    out.append(" != MJ2X_REGC_STATUS_PENDING_EXCEPTION && !_mj2x_regc_exception_check())\n");
    out.append("        mj2x_regc_report_status(_mj2x_regc_current_context(), runtime_status_");
    out.append(std::to_string(temp));
    out.append(", ");
    out.append(cStringLiteral(operation));
    out.append(");\n");
}

ca::str::Utf8String nativeObjectStorageName(RegId id)
{
    CSourceStream os;
    os.append("native_r");
    os.append(std::to_string(id.value));
    return os.build();
}

const RegCNativeObjectState* nativeObjectFor(const RegCFunctionEmitState& state,
                                             const RegOperand&            operand)
{
    if (operand.kind != RegOperandKind::Register) return nullptr;
    auto it = state.nativeObjects.find(operand.reg.value);
    if (it == state.nativeObjects.end()) return nullptr;
    return &it->second;
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
    out.append("    if (");
    out.append(expression);
    out.append(" == NULL && !_mj2x_regc_exception_check()) {\n");
    const std::string statusExpr = codegen::exprCall(
        materializeSymbol,
        {"_mj2x_regc_current_context()",
         stringBuilderStateExpr(operand, objectState->kind).to_std_string(),
         "&" + expression.to_std_string()});
    emitRegCStatusCheck(out, state, ca::str::Utf8StringRef::from_string_view(statusExpr),
                        operation);
    out.append("    }\n");
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
    std::set<uint32_t>                                nativeRegisters;
    std::vector<uint32_t>                             ownerRegisters;
    std::map<uint32_t, uint32_t>                      ownerByRegister;
    std::map<uint32_t, RegCIntrinsicNativeObjectKind> kindByRegister;
};

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

ca::str::Utf8String binaryExpr(const RegInstruction& ins)
{
    const ca::str::Utf8String lhs = operandExpr(ins.lhs);
    const ca::str::Utf8String rhs = operandExpr(ins.rhs);
    const SSAType             type = ins.dest.type;

    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Div || ins.binaryOp == SSABinaryOp::Rem)) {
        const char* op    = ins.binaryOp == SSABinaryOp::Div ? "div" : "rem";
        const char* width = type == SSAType::I64 ? "i64" : "i32";
        CSourceStream os;
        os.append("mj2x_regc_");
        os.append(op);
        os.append("_");
        os.append(width);
        os.append("(_mj2x_regc_current_context(), ");
        os.append(lhs);
        os.append(", ");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    if (ins.binaryOp == SSABinaryOp::UShr) {
        CSourceStream os;
        os.append("(");
        os.append(cType(type));
        os.append(")((");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(lhs);
        os.append(" >> ");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub ||
         ins.binaryOp == SSABinaryOp::Mul)) {
        CSourceStream os;
        os.append("(");
        os.append(cType(type));
        os.append(")((");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(lhs);
        os.append(" ");
        os.append(binaryOp(ins.binaryOp));
        os.append(" (");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    if ((type == SSAType::F32 || type == SSAType::F64) && ins.binaryOp == SSABinaryOp::Rem) {
        CSourceStream os;
        os.append(type == SSAType::F32 ? "fmodf" : "fmod");
        os.append("(");
        os.append(lhs);
        os.append(", ");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    CSourceStream os;
    os.append("(");
    os.append(cType(type));
    os.append(")(");
    os.append(lhs);
    os.append(" ");
    os.append(binaryOp(ins.binaryOp));
    os.append(" ");
    os.append(rhs);
    os.append(")");
    return os.build();
}

void emitHelperPrelude(ca::str::Utf8StringBuilder& out)
{
    static const char* const kRegCHelperPreludeDeclarations[] = {
        "static mj2x_regc_context* _mj2x_regc_current_context(void);\n",
        "static float _mj2x_regc_f32_from_bits(uint32_t bits);\n",
        "static double _mj2x_regc_f64_from_bits(uint64_t bits);\n",
        "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc);\n",
        "static mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache);\n",
        "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length);\n",
        "static mj2x_ref _mj2x_regc_alloc_ref_array_cached(mj2x_regc_class_cache* "
        "component_cache, int32_t length);\n",
        "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, "
        "int32_t* dims);\n",
        "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref);\n",
        "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc);\n",
        "static mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, "
        "mj2x_regc_class_cache* cache);\n",
        "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc);\n",
        "static int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, "
        "mj2x_regc_class_cache* cache);\n",
        "static void _mj2x_regc_monitor_enter(mj2x_ref value);\n",
        "static void _mj2x_regc_monitor_exit(mj2x_ref value);\n",
        "static int32_t _mj2x_regc_exception_check(void);\n",
        "static mj2x_ref _mj2x_regc_exception_occurred(void);\n",
        "static void _mj2x_regc_exception_clear(void);\n",
        "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type);\n",
        "static void _mj2x_regc_throw(mj2x_ref value);\n",
        "static bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache);\n",
        "static int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache);\n",
        "static uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* cache);\n",
        "static int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache);\n",
        "static int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache);\n",
        "static int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache);\n",
        "static float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache);\n",
        "static double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache);\n",
        "static mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* cache);\n",
        "static bool _mj2x_regc_load_cached_instance_bool_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int8_t _mj2x_regc_load_cached_instance_i8_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static uint16_t _mj2x_regc_load_cached_instance_char_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int16_t _mj2x_regc_load_cached_instance_i16_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int32_t _mj2x_regc_load_cached_instance_i32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int64_t _mj2x_regc_load_cached_instance_i64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static float _mj2x_regc_load_cached_instance_f32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static double _mj2x_regc_load_cached_instance_f64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static mj2x_ref _mj2x_regc_load_cached_instance_ref_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, bool value);\n",
        "static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int8_t value);\n",
        "static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, uint16_t value);\n",
        "static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int16_t value);\n",
        "static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int32_t value);\n",
        "static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int64_t value);\n",
        "static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, float value);\n",
        "static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, double value);\n",
        "static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, mj2x_ref value);\n",
        "static void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, "
        "bool value);\n",
        "static void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, "
        "int8_t value);\n",
        "static void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, "
        "uint16_t value);\n",
        "static void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, "
        "int16_t value);\n",
        "static void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, "
        "int32_t value);\n",
        "static void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, "
        "int64_t value);\n",
        "static void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, "
        "float value);\n",
        "static void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, "
        "double value);\n",
        "static void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, "
        "mj2x_ref value);\n",
        "static void _mj2x_regc_store_cached_instance_bool_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value);\n",
        "static void _mj2x_regc_store_cached_instance_i8_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value);\n",
        "static void _mj2x_regc_store_cached_instance_char_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i16_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value);\n",
        "static void _mj2x_regc_store_cached_instance_f32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value);\n",
        "static void _mj2x_regc_store_cached_instance_f64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value);\n",
        "static void _mj2x_regc_store_cached_instance_ref_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value);\n",
        "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index);\n",
        "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index);\n",
        "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index);\n",
        "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index);\n",
        "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index);\n",
        "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index);\n",
        "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index);\n",
        "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index);\n",
        "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t "
        "value);\n",
        "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t "
        "value);\n",
        "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t "
        "value);\n",
        "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t "
        "value);\n",
        "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t "
        "value);\n",
        "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float "
        "value);\n",
        "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double "
        "value);\n",
        "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref "
        "value);\n",
        "static int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_f32(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static double _mj2x_regc_call_f64(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_void(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static int32_t _mj2x_regc_call_cached_instance_i32("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_cached_instance_i64("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_cached_instance_f32("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static double _mj2x_regc_call_cached_instance_f64("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_cached_instance_ref("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_cached_instance_void("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
    };
    for (const char* declaration : kRegCHelperPreludeDeclarations) {
        out.append(declaration);
    }
    RegCIntrinsicRegistry::builtins().emitDeclarations(out);
}

ca::str::Utf8String callArgs(const std::vector<RegOperand>& args)
{
    CSourceStream os;
    os.append(std::to_string(args.size()));
    for (const auto& arg : args) {
        os.append(", (");
        os.append(cType(arg.type));
        os.append(")(");
        os.append(operandExpr(arg));
        os.append(")");
    }
    return os.build();
}

ca::str::Utf8String callArgs(const std::vector<RegOperand>& args,
                             const std::vector<ca::str::Utf8String>& expressions)
{
    CSourceStream os;
    os.append(std::to_string(args.size()));
    for (size_t i = 0; i < args.size(); ++i) {
        os.append(", (");
        os.append(cType(args[i].type));
        os.append(")(");
        os.append(expressions[i]);
        os.append(")");
    }
    return os.build();
}

ca::str::Utf8String directCallArgs(const std::vector<RegOperand>& args)
{
    CSourceStream os;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) os.append(", ");
        os.append("(");
        os.append(cType(args[i].type));
        os.append(")(");
        os.append(operandExpr(args[i]));
        os.append(")");
    }
    return os.build();
}

ca::str::Utf8String directCallArgs(const std::vector<RegOperand>& args,
                                   const std::vector<ca::str::Utf8String>& expressions)
{
    CSourceStream os;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) os.append(", ");
        os.append("(");
        os.append(cType(args[i].type));
        os.append(")(");
        os.append(expressions[i]);
        os.append(")");
    }
    return os.build();
}

bool isSelfStaticCall(const RegCFunctionEmitState& state, const RegInstruction& ins)
{
    return isStaticCall(ins) &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.owner) == ins.method.owner &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.name) == ins.method.name &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.desc) == ins.method.desc;
}

std::string cacheKey(const ca::str::Utf8StringRef& first, const ca::str::Utf8StringRef& second,
                     const ca::str::Utf8StringRef& third)
{
    return first.to_std_string() + "\n" + second.to_std_string() + "\n" + third.to_std_string();
}

ca::str::Utf8String addStaticMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticMethodCache(
            ins.method.owner, ins.method.name, ins.method.desc);
    }

    const std::string key = cacheKey(ins.method.owner, ins.method.name, ins.method.desc);
    auto              it  = state.staticMethodCacheNames.find(key);
    if (it != state.staticMethodCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(std::to_string(state.staticMethodCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: static ");
        decl.append(codegen::escapeCommentIdent(ins.method.owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(ins.method.name));
        decl.append(codegen::escapeCommentIdent(ins.method.desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(ins.method.owner));
    decl.append(", ");
    decl.append(cStringLiteral(ins.method.name));
    decl.append(", ");
    decl.append(cStringLiteral(ins.method.desc));
    decl.append(", NULL, NULL};\n");
    state.staticMethodCacheDeclarations.push_back(decl.build());
    state.staticMethodCacheNames.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String addInstanceMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8String();
    return state.cachePool->instanceMethodCache(ins.method.owner, ins.method.name, ins.method.desc);
}

/// 判定调用是否确定性分派：静态/特殊调用总是确定；虚调用仅当目标方法 final
/// 或 owner 类 final（无子类可覆盖）时确定。接口调用保持动态分派。
bool isDeterministicDispatch(const RegInstruction& ins, CompilerIndex* classIndex)
{
    switch (ins.method.invokeOpcode) {
    case Opcodes::INVOKESTATIC:
    case Opcodes::INVOKESPECIAL:
        return true;
    case Opcodes::INVOKEVIRTUAL:
        break;
    default:
        return false;
    }
    if (classIndex == nullptr) return false;
    const CompilerClass* ownerClass =
        classIndex->getClass(ins.method.owner.to_std_string());
    if (ownerClass == nullptr || ownerClass->classNode == nullptr) return false;
    if ((ownerClass->classNode->access & Opcodes::ACC_FINAL) != 0) return true;
    for (const auto& member : ownerClass->methods) {
        if (member.methodNode == nullptr) continue;
        if (member.methodNode->name != ins.method.name) continue;
        if (member.methodNode->desc != ins.method.desc) continue;
        return (member.methodNode->access & Opcodes::ACC_FINAL) != 0;
    }
    return false;
}

/// 发射 native-to-native 直接调用（跳过 JNI 方法分派）。要求：目标方法已成功
/// 转换、分派确定、无 invokedynamic 站点、无 native 状态参数逃逸。未命中时
/// 返回 false，调用方继续走 intrinsic/通用 JNI 路径。
bool tryEmitDirectCall(CSourceStream& out, const RegInstruction& ins,
                       RegCFunctionEmitState& state, CompilerIndex* classIndex,
                       const std::unordered_map<std::string, std::string>* convertedFunctions,
                       const std::unordered_set<std::string>* indyFunctions)
{
    if (convertedFunctions == nullptr) return false;
    if (isSelfStaticCall(state, ins)) return false;
    const std::string name = ins.method.name.to_std_string();
    if (name.empty() || name[0] == '<') return false;   // <init>/<clinit> 保持 JNI
    const std::string key = ins.method.owner.to_std_string() + "\n" + name + "\n" +
                            ins.method.desc.to_std_string();
    const auto it = convertedFunctions->find(key);
    if (it == convertedFunctions->end() || it->second.empty()) return false;
    if (!isDeterministicDispatch(ins, classIndex)) return false;
    if (indyFunctions != nullptr && indyFunctions->count(key) != 0) return false;
    // native 状态参数在寄存器里是 NULL，直接调用会破坏被调方 ABI；回退通用路径。
    for (const auto& arg : ins.args) {
        if (nativeObjectFor(state, arg) != nullptr) return false;
    }
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
    out.append("    ");
    if (ins.hasDest) {
        out.append(destName);
        out.append(" = ");
    }
    out.append(it->second);
    out.append("(");
    out.append(directCallArgs(ins.args));
    out.append(");\n");
    return true;
}

/// 发射缓存化的 JNI 调用点。与 varargs 帮手相比省去了每次调用的描述符
/// 扫描（_mj2x_regc_count_params / _mj2x_regc_skip_type）与 va_list 搬运，
/// 直接按已知参数类型填充 jvalue 数组并走 CallXxxMethodA。行为与原有
/// _mj2x_regc_call_cached_* 帮手一致：方法/类解析失败时目标寄存器落默认值，
/// 实例调用 receiver 为 NULL 时抛 NullPointerException。
void emitCachedCall(CSourceStream& out, const RegInstruction& ins, bool staticCall,
                    const ca::str::Utf8StringRef& cacheName,
                    const std::vector<ca::str::Utf8String>& expressions)
{
    const char               returnTag = descriptorReturnTag(ins.method.desc);
    const std::vector<char>  paramTags = descriptorParamTags(ins.method.desc);
    const bool               hasDest   = ins.hasDest;
    const std::string        destName  = hasDest ? regName(ins.dest.id) : std::string();
    const ca::str::Utf8String argsArg  = paramTags.empty() ? ca::str::Utf8String("NULL")
                                                          : ca::str::Utf8String("_mj2x_regc_va");

    out.append("    {\n");
    if (!staticCall) {
        out.append("        jobject _mj2x_regc_rcv = ");
        out.append(codegen::exprCast("jobject",
                                     codegen::exprParen(expressions[0].to_std_string())));
        out.append(";\n");
    }
    if (!paramTags.empty()) {
        out.append("        jvalue _mj2x_regc_va[");
        out.append(std::to_string(paramTags.size()));
        out.append("];\n");
    }
    out.append("        jmethodID _mj2x_regc_m;\n");
    if (staticCall) {
        for (size_t i = 0; i < paramTags.size(); ++i) {
            out.append("        ");
            out.append(codegen::exprMember(codegen::exprIndex("_mj2x_regc_va", std::to_string(i)),
                                           jvalueSlotName(paramTags[i])));
            out.append(" = ");
            out.append(codegen::exprCast(jniValueCast(paramTags[i]),
                                         codegen::exprParen(expressions[i].to_std_string())));
            out.append(";\n");
        }
        out.append("        _mj2x_regc_m = ");
        out.append(codegen::exprCall("_mj2x_regc_cached_static_method",
                                     {"&" + cacheName.to_std_string()}));
        out.append(";\n");
        out.append("        if (");
        out.append(codegen::exprBinop(
            "&&", codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
            codegen::exprBinop("!=", codegen::exprMember(cacheName.to_std_string(), "ownerClass"),
                               "NULL")));
        out.append(") {\n");
        const std::string callExpr = codegen::exprCall(
            std::string("(*_mj2x_regc_env)->CallStatic") + jniCallFamily(returnTag) + "MethodA",
            {"_mj2x_regc_env", codegen::exprMember(cacheName.to_std_string(), "ownerClass"),
             "_mj2x_regc_m", argsArg.to_std_string()});
        if (hasDest) {
            out.append("            ");
            out.append(destName);
            out.append(" = ");
            out.append(jniResultCast(returnTag, ins.dest.type));
            out.append(callExpr);
            out.append(";\n");
        }
        else {
            out.append("            ");
            out.append(callExpr);
            out.append(";\n");
        }
        out.append("        } else {\n");
        if (hasDest) {
            out.append("            ");
            out.append(destName);
            out.append(" = ");
            out.append(defaultValue(ins.dest.type));
            out.append(";\n");
        }
        out.append("        }\n");
    }
    else {
        out.append("        if (");
        out.append(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"));
        out.append(") {\n");
        out.append("            _mj2x_regc_throw_null(\"instance method call\");\n");
        if (hasDest) {
            out.append("            ");
            out.append(destName);
            out.append(" = ");
            out.append(defaultValue(ins.dest.type));
            out.append(";\n");
        }
        out.append("        } else {\n");
        for (size_t i = 0; i < paramTags.size(); ++i) {
            out.append("            ");
            out.append(codegen::exprMember(codegen::exprIndex("_mj2x_regc_va", std::to_string(i)),
                                           jvalueSlotName(paramTags[i])));
            out.append(" = ");
            out.append(codegen::exprCast(jniValueCast(paramTags[i]),
                                         codegen::exprParen(expressions[i + 1].to_std_string())));
            out.append(";\n");
        }
        out.append("            _mj2x_regc_m = ");
        out.append(codegen::exprCall("_mj2x_regc_cached_instance_method",
                                     {"&" + cacheName.to_std_string(), "_mj2x_regc_rcv"}));
        out.append(";\n");
        out.append("            if (");
        out.append(codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"));
        out.append(") {\n");
        const std::string callExpr = codegen::exprCall(
            std::string("(*_mj2x_regc_env)->Call") + jniCallFamily(returnTag) + "MethodA",
            {"_mj2x_regc_env", "_mj2x_regc_rcv", "_mj2x_regc_m", argsArg.to_std_string()});
        if (hasDest) {
            out.append("                ");
            out.append(destName);
            out.append(" = ");
            out.append(jniResultCast(returnTag, ins.dest.type));
            out.append(callExpr);
            out.append(";\n");
        }
        else {
            out.append("                ");
            out.append(callExpr);
            out.append(";\n");
        }
        out.append("            } else {\n");
        if (hasDest) {
            out.append("                ");
            out.append(destName);
            out.append(" = ");
            out.append(defaultValue(ins.dest.type));
            out.append(";\n");
        }
        out.append("            }\n");
        out.append("        }\n");
    }
    out.append("    }\n");
}

/// 发射动态实例调用的 resolved native dispatch：按 receiver 实际类解析 jmethodID，
/// 若该调用点解析出的实现是已转换方法则直接调用其 regc_* 函数体（跳过 JNI
/// 分派），否则回退 CallXxxMethodA。站点级缓存持有 receiver 类 global ref，
/// 同类重复调用只做 GetObjectClass + IsSameObject 检查。
void emitResolvedInstanceCall(CSourceStream& out, const RegInstruction& ins,
                              const ca::str::Utf8StringRef& siteName,
                              const std::vector<ca::str::Utf8String>& expressions)
{
    const char              returnTag = descriptorReturnTag(ins.method.desc);
    const std::vector<char> paramTags = descriptorParamTags(ins.method.desc);
    const bool              hasDest   = ins.hasDest;
    const std::string       destName  = hasDest ? regName(ins.dest.id) : std::string();
    const std::string       site      = siteName.to_std_string();
    const std::string       argsArg   = paramTags.empty() ? std::string("NULL")
                                                         : std::string("_mj2x_regc_va");

    // 函数指针签名与实参列表（直接调用专用）：与 regc_* 函数体 ABI 一致。
    std::string fnPointerSig = std::string(cType(ins.dest.type)) + " (*)(";
    std::vector<std::string> callArguments;
    callArguments.push_back(codegen::exprCast("mj2x_ref", codegen::exprParen("_mj2x_regc_rcv")));
    fnPointerSig += "mj2x_ref";
    for (size_t i = 0; i < paramTags.size(); ++i) {
        fnPointerSig += ", ";
        fnPointerSig += cType(ins.args[i + 1].type);
        callArguments.push_back(codegen::exprCast(
            cType(ins.args[i + 1].type), codegen::exprParen(expressions[i + 1].to_std_string())));
    }
    fnPointerSig += ")";
    // void 返回值用 void 签名（hasDest=false 时 cType 也是 "void"，但显式构建
    // 更清晰，避免从 dest 签名里抠参数子串）。
    std::string voidPointerSig = "void (*)(";
    voidPointerSig += "mj2x_ref";
    for (size_t i = 0; i < paramTags.size(); ++i) {
        voidPointerSig += ", ";
        voidPointerSig += cType(ins.args[i + 1].type);
    }
    voidPointerSig += ")";

    const std::string fnPtrCallExpr =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(fnPointerSig,
                                                                  codegen::exprMember(site, "fn"))),
                              callArguments);
    const std::string voidFnPtrCallExpr =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(voidPointerSig,
                                                                  codegen::exprMember(site, "fn"))),
                              callArguments);
    const std::string jniCallExpr = codegen::exprCall(
        std::string("(*_mj2x_regc_env)->Call") + jniCallFamily(returnTag) + "MethodA",
        {"_mj2x_regc_env", "_mj2x_regc_rcv", "_mj2x_regc_m", argsArg});

    out.block([&](CSourceStream& o) {
        o.emitLine("jobject _mj2x_regc_rcv = " +
                   codegen::exprCast("jobject",
                                     codegen::exprParen(expressions[0].to_std_string())) +
                   ";");
        o.iff(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"),
              [&](CSourceStream& s) {
                  s.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                  if (hasDest) {
                      s.emitLine(destName + " = " + std::string(defaultValue(ins.dest.type)) + ";");
                  }
              },
              [&](CSourceStream& s) {
                  s.emitLine("jclass _mj2x_regc_rc = " +
                             codegen::exprCall("(*_mj2x_regc_env)->GetObjectClass",
                                               {"_mj2x_regc_env", "_mj2x_regc_rcv"}) +
                             ";");
                  s.emitLine("jmethodID _mj2x_regc_m = NULL;");
                  if (!paramTags.empty()) {
                      s.emitLine("jvalue _mj2x_regc_va[" + std::to_string(paramTags.size()) +
                                 "];");
                  }
                  s.iff(codegen::exprBinop("!=", "_mj2x_regc_rc", "NULL"),
                        [&](CSourceStream& t) {
                            // 快路径 1（单 JNI）：receiver 是已转换实现类（或其未覆盖
                            // 子类）→ 直接复用 method/fn。implClass 只在 fn != NULL 时设置。
                            t.iff(codegen::exprBinop(
                                      "&&",
                                      codegen::exprBinop("!=",
                                                         codegen::exprMember(site, "implClass"),
                                                         "NULL"),
                                      codegen::exprCall("(*_mj2x_regc_env)->IsInstanceOf",
                                                        {"_mj2x_regc_env", "_mj2x_regc_rcv",
                                                         codegen::exprMember(site, "implClass")})),
                                  [&](CSourceStream& u) {
                                      u.emitLine("_mj2x_regc_m = " +
                                                 codegen::exprMember(site, "method") + ";");
                                  },
                                  [&](CSourceStream& t2) {
                                      // 快路径 2（双 JNI）：无已转换实现（fn==NULL）的调用
                                      // 点按 receiver 精确类缓存 jmethodID。
                                      t2.iff(codegen::exprBinop(
                                                 "&&",
                                                 codegen::exprBinop(
                                                     "!=",
                                                     codegen::exprMember(site, "receiverClass"),
                                                     "NULL"),
                                                 codegen::exprCall(
                                                     "(*_mj2x_regc_env)->IsSameObject",
                                                     {"_mj2x_regc_env",
                                                      codegen::exprMember(site, "receiverClass"),
                                                      "_mj2x_regc_rc"})),
                                             [&](CSourceStream& u) {
                                                 u.emitLine("_mj2x_regc_m = " +
                                                            codegen::exprMember(site, "method") +
                                                            ";");
                                             },
                                             [&](CSourceStream& u) {
                                                 u.emitLine(
                                                     "_mj2x_regc_m = " +
                                                     codegen::exprCall(
                                                         "_mj2x_regc_resolved_call_site_refresh",
                                                         {"&" + site, "_mj2x_regc_rcv",
                                                          "_mj2x_regc_rc",
                                                          cStringLiteral(ins.method.name),
                                                          cStringLiteral(ins.method.desc)}) +
                                                     ";");
                                             });
                                  });
                            t.emitLine(codegen::exprCall("(*_mj2x_regc_env)->DeleteLocalRef",
                                                        {"_mj2x_regc_env", "_mj2x_regc_rc"}) +
                                       ";");
                        });
                  for (size_t i = 0; i < paramTags.size(); ++i) {
                      s.emitLine(codegen::exprMember(
                                     codegen::exprIndex("_mj2x_regc_va", std::to_string(i)),
                                     jvalueSlotName(paramTags[i])) +
                                 " = " +
                                 codegen::exprCast(jniValueCast(paramTags[i]),
                                                   codegen::exprParen(
                                                       expressions[i + 1].to_std_string())) +
                                 ";");
                  }
                  if (hasDest) {
                      s.iff(codegen::exprBinop("==", "_mj2x_regc_m", "NULL"),
                            [&](CSourceStream& t) {
                                t.emitLine(destName + " = " +
                                           std::string(defaultValue(ins.dest.type)) + ";");
                            },
                            [&](CSourceStream& t) {
                                t.iff(codegen::exprBinop("!=", codegen::exprMember(site, "fn"),
                                                         "NULL"),
                                      [&](CSourceStream& u) {
                                          u.emitLine(destName + " = " +
                                                     codegen::exprCast(
                                                         jniResultCast(returnTag, ins.dest.type)
                                                             .to_std_string(),
                                                         fnPtrCallExpr) +
                                                     ";");
                                      },
                                      [&](CSourceStream& u) {
                                          u.emitLine(destName + " = " +
                                                     codegen::exprCast(
                                                         jniResultCast(returnTag, ins.dest.type)
                                                             .to_std_string(),
                                                         jniCallExpr) +
                                                     ";");
                                      });
                            });
                  }
                  else {
                      s.iff(codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
                            [&](CSourceStream& t) {
                                t.iff(codegen::exprBinop("!=", codegen::exprMember(site, "fn"),
                                                         "NULL"),
                                      [&](CSourceStream& u) {
                                          u.emitLine(voidFnPtrCallExpr + ";");
                                      },
                                      [&](CSourceStream& u) {
                                          u.emitLine(jniCallExpr + ";");
                                      });
                            });
                  }
              });
    });
}

ca::str::Utf8String addStaticFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
    }

    const std::string key = cacheKey(ins.field.owner, ins.field.name, ins.field.desc);
    auto              it  = state.staticFieldCacheNames.find(key);
    if (it != state.staticFieldCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_field_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(std::to_string(state.staticFieldCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: static field ");
        decl.append(codegen::escapeCommentIdent(ins.field.owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(ins.field.name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(ins.field.desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(ins.field.owner));
    decl.append(", ");
    decl.append(cStringLiteral(ins.field.name));
    decl.append(", ");
    decl.append(cStringLiteral(ins.field.desc));
    decl.append(", NULL, NULL};\n");
    state.staticFieldCacheDeclarations.push_back(decl.build());
    state.staticFieldCacheNames.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String addInstanceFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8String();
    return state.cachePool->instanceFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
}

ca::str::Utf8String addClassCache(RegCFunctionEmitState& state, const ca::str::Utf8StringRef& desc)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->classCache(desc);
    }

    const std::string descKey = desc.to_std_string();
    auto              it      = state.classCacheNames.find(descKey);
    if (it != state.classCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_class_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(std::to_string(state.classCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: class ");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_class_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL};\n");
    state.classCacheDeclarations.push_back(decl.build());
    state.classCacheNames.emplace(descKey, cacheName.clone());
    return cacheName;
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
        out.append("    ");
        out.append(nativeObjectType(decision.nativeObjectKind));
        out.append(" ");
        out.append(nativeObjectStorageName(ins.dest.id));
        out.append(" = ");
        std::vector<std::string> valueOfArgs;
        if (!ins.args.empty()) valueOfArgs.push_back(operandExpr(ins.args[0]).to_std_string());
        out.append(codegen::exprCallArgs(decision.symbol, valueOfArgs));
        out.append(";\n");
        const ca::str::Utf8String storageName = nativeObjectStorageName(ins.dest.id);
        const std::string         destName    = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = NULL;\n");
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
        out.append("    ");
        if (ins.hasDest) {
            const std::string name = regName(ins.dest.id);
            out.append(name.data(), name.size());
            out.append(" = ");
        }
        out.append(codegen::exprCall(decision.symbol, {objectState->storageName}));
        out.append(";\n");
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
        out.append("    ");
        out.append(codegen::exprCall(decision.symbol,
                                     {"&" + objectState->storageName,
                                      operandExpr(ins.args[1]).to_std_string()}));
        out.append(";\n");
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
        out.append("    ");
        if (ins.hasDest) {
            const std::string destName = regName(ins.dest.id);
            out.append(destName.data(), destName.size());
            out.append(" = ");
        }
        std::vector<std::string> nativeArgs;
        nativeArgs.push_back("&" + objectState->storageName);
        for (size_t i = 1; i < ins.args.size(); ++i) {
            nativeArgs.push_back(operandExpr(ins.args[i]).to_std_string());
        }
        out.append(codegen::exprCallArgs(decision.symbol, nativeArgs));
        out.append(";\n");
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
            out.append("    ");
            out.append(resultType);
            out.append(" mutable_string_value_");
            out.append(std::to_string(valueTemp));
            out.append(" = ");
            out.append(defaultValue(ins.dest.type));
            out.append(";\n");
            callArgs.push_back("&mutable_string_value_" + std::to_string(valueTemp));
        }
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out,
                            state,
                            ca::str::Utf8StringRef::from_string_view(callText),
                            std::string(mutableStringDisplayName(objectState->kind)) + ".method");
        if (ins.hasDest) {
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = mutable_string_value_");
            out.append(std::to_string(valueTemp));
            out.append(";\n");
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
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = ");
            out.append(operandExpr(ins.args[0]));
            out.append(";\n");
            state.nativeObjects[ins.dest.id.value] =
                RegCNativeObjectState{objectState->kind, destName};
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
        out.append("    ");
        out.append(cType(ins.dest.type));
        out.append(" string_value_");
        out.append(std::to_string(valueTemp));
        out.append(" = ");
        out.append(defaultValue(ins.dest.type));
        out.append(";\n");
        std::vector<std::string> callArgs;
        callArgs.push_back("_mj2x_regc_current_context()");
        for (const auto& arg : ins.args) {
            callArgs.push_back(operandExpr(arg).to_std_string());
        }
        callArgs.push_back("&string_value_" + std::to_string(valueTemp));
        const std::string callText = codegen::exprCallArgs(decision.symbol, callArgs);
        emitRegCStatusCheck(out, state, ca::str::Utf8StringRef::from_string_view(callText),
                            "String intrinsic");
        const std::string destName = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = string_value_");
        out.append(std::to_string(valueTemp));
        out.append(";\n");
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
        out.append("    ");
        if (ins.hasDest) {
            out.append(destName.data(), destName.size());
            out.append(" = ");
        }
        out.append(codegen::exprCallArgs(decision.symbol, stringArgs));
        out.append(";\n");
        return decision;
    }

    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
    std::vector<std::string> fallbackArgs;
    fallbackArgs.reserve(ins.args.size() + 1);
    if (decision.passContext) fallbackArgs.push_back("_mj2x_regc_current_context()");
    for (const auto& arg : ins.args) {
        fallbackArgs.push_back(operandExpr(arg).to_std_string());
    }
    out.append("    ");
    if (ins.hasDest) {
        out.append(destName.data(), destName.size());
        out.append(" = ");
    }
    out.append(codegen::exprCallArgs(decision.symbol, fallbackArgs));
    out.append(";\n");
    return decision;
}

bool emitEdgeCopies(CSourceStream& out, const std::vector<RegCopy>& copies,
                    RegCFunctionEmitState& state, int& edgeTempIndex, std::string& reason)
{
    if (copies.empty()) return true;

    // Edge copies are parallel phi transfers.  Stage sources in temporaries so
    // cycles such as r1<-r2 and r2<-r1 remain well-defined for later RegC work.
    std::vector<int> tempIds;
    tempIds.reserve(copies.size());
    for (size_t i = 0; i < copies.size(); ++i) {
        const RegCNativeObjectState* objectState = nativeObjectFor(state, copies[i].source);
        if (objectState != nullptr && !isMutableStringKind(objectState->kind)) {
            reason = "native object escaped through CFG edge copy";
            return false;
        }
        const int tempId = edgeTempIndex++;
        tempIds.push_back(tempId);
        // Record the actual type per emitted temporary so the declaration block
        // declares exactly the temporaries that are used. A block with N mayThrow
        // instructions re-emits its exception edge copies N times, so the emit count
        // can exceed the static CFG edge count that collectEdgeCopyTempTypes sees.
        if (static_cast<int>(state.edgeTempTypes.size()) <= tempId) {
            state.edgeTempTypes.resize(static_cast<size_t>(tempId) + 1, copies[i].dest.type);
        }
        out.append("    edge_tmp_");
        out.append(std::to_string(tempId));
        out.append(" = (");
        out.append(cType(copies[i].dest.type));
        out.append(")(");
        out.append(operandExpr(copies[i].source));
        out.append(");\n");
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        const std::string destName = regName(copies[i].dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = edge_tmp_");
        out.append(std::to_string(tempIds[i]));
        out.append(";\n");
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

void emitDefaultReturnAfterException(CSourceStream& out, SSAType returnType)
{
    if (returnType == SSAType::VOID) {
        out.append("        goto _mj2x_regc_cleanup;\n");
    }
    else {
        out.append("        _mj2x_regc_result = ");
        out.append(defaultValue(returnType));
        out.append(";\n");
        out.append("        goto _mj2x_regc_cleanup;\n");
    }
}

/// 判断已发射的方法体是否含产生 JNI local ref 的操作，用于 gate PushLocalFrame/
/// PopLocalFrame 帧管理。命中任一产 ref helper 即需要帧；纯标量/纯字段算术方法
/// 不含这些标记，保留「零 JNI 帧开销」形态。marker 覆盖：方法调用（call_*，含
/// cached / instance / ref / void——void 调用也可能内部 FindClass 泄漏 class ref）、
/// 对象/数组分配、class/string 字面量、ref 字段与 ref 数组读取。
bool bodyProducesLocalRefs(const ca::str::Utf8StringRef& body)
{
    static const char* kMarkers[] = {
        "_mj2x_regc_call_",          // 所有方法调用（含 static FindClass 路径）
        "_mj2x_regc_alloc_",         // alloc_object / alloc_array / alloc_multi_array
        "_mj2x_regc_class_literal",  // Foo.class -> FindClass
        "_mj2x_intrinsic_string_literal",
        "_mj2x_regc_load_ref_field",
        "_mj2x_regc_load_cached_ref_field",
        "_mj2x_regc_load_ref_array",
    };
    for (const char* marker : kMarkers) {
        if (body.contains(ca::str::Utf8StringRef::from_cstr(marker))) return true;
    }
    return false;
}

bool emitExceptionDispatch(CSourceStream&                        out,
                           const std::vector<const RegExceptionRegion*>&     regions,
                           const RegBlock* block, SSAType returnType,
                           RegCFunctionEmitState& state, int& dispatchIndex,
                           int& edgeTempIndex, std::string& reason)
{
    const int index = dispatchIndex++;
    out.append("    if (_mj2x_regc_exception_check()) {\n");
    if (regions.empty()) {
        emitDefaultReturnAfterException(out, returnType);
        out.append("    }\n");
        return true;
    }
    out.append("        mj2x_ref ex_");
    out.append(std::to_string(index));
    out.append(" = _mj2x_regc_exception_occurred();\n");
    out.append("        _mj2x_regc_exception_clear();\n");
    // Match in exception-table order. A matched handler receives the caught
    // exception through its synthetic exception register before the jump.
    for (const auto* region : regions) {
        out.append("        if (_mj2x_regc_exception_matches(ex_");
        out.append(std::to_string(index));
        out.append(", ");
        out.append(cStringLiteral(region->caughtType));
        out.append(")) {\n");
        for (const auto& edge : region->edges) {
            if (edge.protectedBlock != block) continue;
            if (!emitEdgeCopies(out, edge.copies, state, edgeTempIndex, reason)) return false;
            break;
        }
        const std::string exceptionName = regName(region->exceptionValue.id);
        out.append("            ");
        out.append(exceptionName.data(), exceptionName.size());
        out.append(" = ex_");
        out.append(std::to_string(index));
        out.append(";\n");
        out.append("            goto block_b");
        out.append(std::to_string(region->handler->id.value));
        out.append(";\n");
        out.append("        }\n");
    }
    // 若没有任何 handler 接受挂起的异常，保持 Java 传播语义。
    out.append("        _mj2x_regc_throw(ex_");
    out.append(std::to_string(index));
    out.append(");\n");
    emitDefaultReturnAfterException(out, returnType);
    out.append("    }\n");
    return true;
}

bool emitInstruction(CSourceStream& out, const RegInstruction& ins,
                     RegCFunctionEmitState& state, const RegFunction& fn, size_t indySiteBase,
                     std::string& reason)
{
    if (!isSupportedInstruction(ins, reason)) return false;

    switch (ins.kind) {
    case RegInstructionKind::Copy:
        if (const auto* objectState = nativeObjectFor(state, ins.copy.source)) {
            if (isMutableStringKind(objectState->kind)) {
                const std::string destName = regName(ins.copy.dest.id);
                out.append("    ");
                out.append(destName.data(), destName.size());
                out.append(" = ");
                out.append(operandExpr(ins.copy.source));
                out.append(";\n");
                state.nativeObjects[ins.copy.dest.id.value] =
                    RegCNativeObjectState{objectState->kind, destName};
            }
            else {
                const ca::str::Utf8String storageName = nativeObjectStorageName(ins.copy.dest.id);
                out.append("    ");
                out.append(nativeObjectType(objectState->kind));
                out.append(" ");
                out.append(storageName);
                out.append(" = ");
                out.append(objectState->storageName.data(), objectState->storageName.size());
                out.append(";\n");
                const std::string destName = regName(ins.copy.dest.id);
                out.append("    ");
                out.append(destName.data(), destName.size());
                out.append(" = NULL;\n");
                state.nativeObjects[ins.copy.dest.id.value] =
                    RegCNativeObjectState{objectState->kind, storageName.to_std_string()};
            }
        }
        else {
            state.nativeObjects.erase(ins.copy.dest.id.value);
            const std::string destName = regName(ins.copy.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = (");
            out.append(cType(ins.copy.dest.type));
            out.append(")(");
            out.append(operandExpr(ins.copy.source));
            out.append(");\n");
        }
        return true;
    case RegInstructionKind::Unary:
    {
        const std::string destName = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = (");
        out.append(cType(ins.dest.type));
        out.append(")(-");
        out.append(operandExpr(ins.value));
        out.append(");\n");
        return true;
    }
    case RegInstructionKind::Binary:
    {
        const std::string destName = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = ");
        out.append(binaryExpr(ins));
        out.append(";\n");
        return true;
    }
    case RegInstructionKind::Compare:
        // REF 比较（acmp / 与 null 比较）：JNI local ref 是句柄，同一对象的两个不同
        // local ref 句柄指针不相等，裸 void* 比较会误判。必须用 IsSameObject 判定对象
        // 同一性（对 NULL 也正确：IsSameObject(x, NULL) 当且仅当 x 为 null 时为真）。
        // acmp 在字节码只有 eq/ne 两种谓词。
        if (ins.lhs.type == SSAType::REF || ins.rhs.type == SSAType::REF) {
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = (");
            if (ins.comparePredicate == SSAComparePredicate::Ne) out.append("!");
            out.append("(*_mj2x_regc_env)->IsSameObject(_mj2x_regc_env, (jobject)");
            out.append(operandExpr(ins.lhs));
            out.append(", (jobject)");
            out.append(operandExpr(ins.rhs));
            out.append("));\n");
            return true;
        }
    {
        const std::string destName = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = (");
        out.append(operandExpr(ins.lhs));
        out.append(" ");
        out.append(compareOp(ins.comparePredicate));
        out.append(" ");
        out.append(operandExpr(ins.rhs));
        out.append(");\n");
        return true;
    }
    case RegInstructionKind::Convert:
        if (!rejectNativeObjectEscape(state, ins.value, "reference conversion", reason))
            return false;
        if (ins.convertKind == SSAConvertKind::ReferenceCast) {
            const ca::str::Utf8String cacheName = addClassCache(state, ins.referenceTypeDesc);
            const std::string         destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = _mj2x_regc_checkcast_cached(");
            out.append(operandExpr(ins.value));
            out.append(", &");
            out.append(cacheName);
            out.append(");\n");
        }
        else {
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = (");
            out.append(cType(ins.targetType));
            out.append(")(");
            out.append(operandExpr(ins.value));
            out.append(");\n");
        }
        return true;
    case RegInstructionKind::LoadField:
        if (!rejectNativeObjectEscape(state, ins.receiver, "field load receiver", reason))
            return false;
        if (ins.field.isStatic) {
            const ca::str::Utf8String cacheName = addStaticFieldCache(state, ins);
            const std::string         destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = _mj2x_regc_load_cached_");
            out.append(fieldHelperSuffix(ins.field.desc, ins.dest.type));
            out.append("_field(&");
            out.append(cacheName);
            out.append(");\n");
        }
        else {
            const ca::str::Utf8String cacheName = addInstanceFieldCache(state, ins);
            const std::string         destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = _mj2x_regc_load_");
            if (!cacheName.empty()) {
                out.append("cached_instance_");
                out.append(fieldHelperSuffix(ins.field.desc, ins.dest.type));
                out.append("_field(&");
                out.append(cacheName);
                out.append(", ");
                out.append(operandExpr(ins.receiver));
                out.append(");\n");
            }
            else {
                out.append(fieldHelperSuffix(ins.field.desc, ins.dest.type));
                out.append("_field(0, ");
                out.append(operandExpr(ins.receiver));
                out.append(", ");
                out.append(cStringLiteral(ins.field.owner));
                out.append(", ");
                out.append(cStringLiteral(ins.field.name));
                out.append(", ");
                out.append(cStringLiteral(ins.field.desc));
                out.append(");\n");
            }
        }
        return true;
    case RegInstructionKind::StoreField:
        if (!rejectNativeObjectEscape(state, ins.receiver, "field store receiver", reason))
            return false;
        {
            ca::str::Utf8String valueExpression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
            if (!materialized &&
                !rejectNativeObjectEscape(state, ins.value, "field store value", reason)) {
                return false;
            }
            if (materialized) out.append("    if (!_mj2x_regc_exception_check()) {\n");
            if (ins.field.isStatic) {
                const ca::str::Utf8String cacheName = addStaticFieldCache(state, ins);
                out.append("    _mj2x_regc_store_cached_");
                out.append(fieldHelperSuffix(ins.field.desc, ins.value.type));
                out.append("_field(&");
                out.append(cacheName);
                out.append(", ");
                out.append(valueExpression);
                out.append(");\n");
            }
            else {
                const ca::str::Utf8String cacheName = addInstanceFieldCache(state, ins);
                out.append("    _mj2x_regc_store_");
                if (!cacheName.empty()) {
                    out.append("cached_instance_");
                    out.append(fieldHelperSuffix(ins.field.desc, ins.value.type));
                    out.append("_field(&");
                    out.append(cacheName);
                    out.append(", ");
                    out.append(operandExpr(ins.receiver));
                    out.append(", ");
                    out.append(valueExpression);
                    out.append(");\n");
                }
                else {
                    out.append(fieldHelperSuffix(ins.field.desc, ins.value.type));
                    out.append("_field(0, ");
                    out.append(operandExpr(ins.receiver));
                    out.append(", ");
                    out.append(cStringLiteral(ins.field.owner));
                    out.append(", ");
                    out.append(cStringLiteral(ins.field.name));
                    out.append(", ");
                    out.append(cStringLiteral(ins.field.desc));
                    out.append(", ");
                    out.append(valueExpression);
                    out.append(");\n");
                }
            }
            if (materialized) out.append("    }\n");
            return true;
        }
    case RegInstructionKind::ArrayLoad:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array load receiver", reason))
            return false;
    {
        const std::string destName = regName(ins.dest.id);
        out.append("    ");
        out.append(destName.data(), destName.size());
        out.append(" = _mj2x_regc_load_");
        out.append(arrayHelperSuffix(ins.elementType));
        out.append("_array(");
        out.append(operandExpr(ins.receiver));
        out.append(", ");
        out.append(operandExpr(ins.index));
        out.append(");\n");
        return true;
    }
    case RegInstructionKind::ArrayStore:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array store receiver", reason))
            return false;
        {
            ca::str::Utf8String valueExpression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
            if (!materialized &&
                !rejectNativeObjectEscape(state, ins.value, "array store value", reason)) {
                return false;
            }
            if (materialized) out.append("    if (!_mj2x_regc_exception_check()) {\n");
            out.append("    _mj2x_regc_store_");
            out.append(arrayHelperSuffix(ins.elementType));
            out.append("_array(");
            out.append(operandExpr(ins.receiver));
            out.append(", ");
            out.append(operandExpr(ins.index));
            out.append(", ");
            out.append(valueExpression);
            out.append(");\n");
            if (materialized) out.append("    }\n");
            return true;
        }
    case RegInstructionKind::Call:
    {
        // native-to-native 直接调用优先（确定性分派 + 已转换目标），未命中再走
        // intrinsic/通用 JNI 路径。
        if (tryEmitDirectCall(out, ins, state, state.classIndex, state.convertedFunctions,
                              state.indyFunctions)) {
            return true;
        }
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
        std::vector<ca::str::Utf8String> argumentExpressions;
        argumentExpressions.reserve(ins.args.size());
        bool hasMaterializedArgument = false;
        for (const auto& arg : ins.args) {
            ca::str::Utf8String expression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, arg, expression);
            if (!materialized && nativeObjectFor(state, arg) != nullptr) {
                reason = "native object escaped through generic call " +
                         ins.method.owner.to_std_string() + "." + ins.method.name.to_std_string();
                return false;
            }
            hasMaterializedArgument = hasMaterializedArgument || materialized;
            argumentExpressions.push_back(std::move(expression));
        }
        if (hasMaterializedArgument) out.append("    if (!_mj2x_regc_exception_check()) {\n");
        const bool              staticCall = isStaticCall(ins);
        const bool              selfCall   = isSelfStaticCall(state, ins);
        const ca::str::Utf8String cacheName =
            (staticCall && !selfCall) ? addStaticMethodCache(state, ins) : ca::str::Utf8String();
        const ca::str::Utf8String instanceCacheName =
            (!staticCall && !selfCall) ? addInstanceMethodCache(state, ins) : ca::str::Utf8String();
        const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
        const bool dynamicInstance = !staticCall && !selfCall && !instanceCacheName.empty();
        const bool useResolved = dynamicInstance && state.resolvedDispatch && !state.traceCalls;
        const bool inlineCached =
            !selfCall && !useResolved && (staticCall || !instanceCacheName.empty()) &&
            !(state.traceCalls && !staticCall && !ins.hasDest);
        if (useResolved) {
            emitResolvedInstanceCall(out,
                                     ins,
                                     state.cachePool->resolvedCallSite(ins.method.owner,
                                                                       ins.method.name,
                                                                       ins.method.desc),
                                     argumentExpressions);
        }
        else if (inlineCached) {
            emitCachedCall(out, ins, staticCall, staticCall ? cacheName : instanceCacheName,
                           argumentExpressions);
        }
        else if (ins.hasDest) {
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = ");
            if (selfCall) {
                out.append(state.functionName);
                out.append("(");
                out.append(directCallArgs(ins.args, argumentExpressions));
                out.append(");\n");
            }
            else {
                out.append("_mj2x_regc_call_");
                if (staticCall) {
                    out.append("cached_");
                    out.append(helperSuffix(ins.dest.type));
                    out.append("(&");
                    out.append(cacheName);
                    out.append(", ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
                else if (!instanceCacheName.empty()) {
                    out.append("cached_instance_");
                    out.append(helperSuffix(ins.dest.type));
                    out.append("(&");
                    out.append(instanceCacheName);
                    out.append(", ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
                else {
                    out.append(helperSuffix(ins.dest.type));
                    out.append("(");
                    out.append(cStringLiteral(ins.method.owner));
                    out.append(", ");
                    out.append(cStringLiteral(ins.method.name));
                    out.append(", ");
                    out.append(cStringLiteral(ins.method.desc));
                    out.append(", 0, ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
            }
        }
        else {
            out.append("    ");
            if (selfCall) {
                out.append(state.functionName);
                out.append("(");
                out.append(directCallArgs(ins.args, argumentExpressions));
                out.append(");\n");
            }
            else {
                out.append("_mj2x_regc_call_");
                if (staticCall) {
                    out.append("cached_void(&");
                    out.append(cacheName);
                    out.append(", ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
                else if (!instanceCacheName.empty()) {
                    out.append("cached_instance_void(&");
                    out.append(instanceCacheName);
                    out.append(", ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
                else {
                    out.append("void(");
                    out.append(cStringLiteral(ins.method.owner));
                    out.append(", ");
                    out.append(cStringLiteral(ins.method.name));
                    out.append(", ");
                    out.append(cStringLiteral(ins.method.desc));
                    out.append(", 0, ");
                    out.append(callArgs(ins.args, argumentExpressions));
                    out.append(");\n");
                }
            }
        }
        if (hasMaterializedArgument) out.append("    }\n");
        return true;
    }
    case RegInstructionKind::Allocate:
        if (ins.allocationKind == SSAAllocationKind::Object) {
            if (ins.typeDesc == "java/util/Random") {
                out.append("    mj2x_random ");
                out.append(nativeObjectStorageName(ins.dest.id));
                out.append(" = {0};\n");
                const std::string destRandomName = regName(ins.dest.id);
                out.append("    ");
                out.append(destRandomName.data(), destRandomName.size());
                out.append(" = NULL;\n");
                state.nativeObjects[ins.dest.id.value] = RegCNativeObjectState{
                    RegCIntrinsicNativeObjectKind::Random,
                    nativeObjectStorageName(ins.dest.id).to_std_string()};
            }
            else if ((ins.typeDesc == "java/lang/StringBuilder" ||
                      ins.typeDesc == "java/lang/StringBuffer") &&
                     state.nativeStringBuilderRegisters.count(ins.dest.id.value) != 0) {
                const auto nativeIt = state.nativeObjects.find(ins.dest.id.value);
                if (nativeIt == state.nativeObjects.end()) {
                    reason = "mutable string allocation is missing native state plan";
                    return false;
                }
                const std::string sbDestName = regName(ins.dest.id);
                out.append("    ");
                out.append(sbDestName.data(), sbDestName.size());
                out.append(" = (void*)&");
                out.append(stringBuilderOwnerName(ins.dest.id));
                out.append(";\n");
                state.nativeObjects[ins.dest.id.value] =
                    RegCNativeObjectState{nativeIt->second.kind, sbDestName};
            }
            else {
                const std::string allocDestName = regName(ins.dest.id);
                out.append("    ");
                out.append(allocDestName.data(), allocDestName.size());
                out.append(" = ");
                if (state.cachePool != nullptr) {
                    const ca::str::Utf8String cacheName = addClassCache(state, ins.typeDesc);
                    out.append("_mj2x_regc_alloc_object_cached(&");
                    out.append(cacheName);
                    out.append(");\n");
                }
                else {
                    out.append("_mj2x_regc_alloc_object(");
                    out.append(cStringLiteral(ins.typeDesc));
                    out.append(");\n");
                }
            }
        }
        else if (ins.allocationKind == SSAAllocationKind::Array) {
            const RegOperand length =
                ins.args.empty() ? RegOperand::fromInt(0, SSAType::I32) : ins.args[0];
            const std::string allocArrayDest = regName(ins.dest.id);
            if (state.cachePool != nullptr && isReferenceArrayDesc(ins.typeDesc)) {
                const ca::str::Utf8String cacheName = addClassCache(
                    state,
                    ca::str::Utf8StringRef::from_string_view(arrayComponentDesc(ins.typeDesc)));
                out.append("    ");
                out.append(allocArrayDest.data(), allocArrayDest.size());
                out.append(" = _mj2x_regc_alloc_ref_array_cached(&");
                out.append(cacheName);
                out.append(", ");
                out.append(operandExpr(length));
                out.append(");\n");
            }
            else {
                out.append("    ");
                out.append(allocArrayDest.data(), allocArrayDest.size());
                out.append(" = _mj2x_regc_alloc_array(");
                out.append(cStringLiteral(ins.typeDesc));
                out.append(", ");
                out.append(operandExpr(length));
                out.append(");\n");
            }
        }
        else {
            out.append("    {\n");
            out.append("        int32_t dims[");
            out.append(std::to_string(ins.args.size()));
            out.append("];\n");
            for (size_t i = 0; i < ins.args.size(); ++i) {
                out.append("        dims[");
                out.append(std::to_string(i));
                out.append("] = ");
                out.append(operandExpr(ins.args[i]));
                out.append(";\n");
            }
            const std::vector<std::string> componentDescs =
                state.cachePool != nullptr
                    ? multiArrayComponentDescs(ins.typeDesc, ins.args.size())
                    : std::vector<std::string>{};
            if (!componentDescs.empty()) {
                out.append("        mj2x_regc_class_cache* component_caches[");
                out.append(std::to_string(componentDescs.size()));
                out.append("] = {");
                for (size_t i = 0; i < componentDescs.size(); ++i) {
                    if (i != 0) out.append(", ");
                    const ca::str::Utf8String cacheName = addClassCache(
                        state,
                        ca::str::Utf8StringRef::from_string_view(componentDescs[i]));
                    out.append("&");
                    out.append(cacheName);
                }
                out.append("};\n");
                const std::string multiDestName = regName(ins.dest.id);
                out.append("        ");
                out.append(multiDestName.data(), multiDestName.size());
                out.append(" = _mj2x_regc_alloc_multi_array_cached(");
                out.append(cStringLiteral(ins.typeDesc));
                out.append(", ");
                out.append(std::to_string(ins.args.size()));
                out.append(", dims, component_caches);\n");
            }
            else {
                const std::string multiDestName = regName(ins.dest.id);
                out.append("        ");
                out.append(multiDestName.data(), multiDestName.size());
                out.append(" = _mj2x_regc_alloc_multi_array(");
                out.append(cStringLiteral(ins.typeDesc));
                out.append(", ");
                out.append(std::to_string(ins.args.size()));
                out.append(", dims);\n");
            }
            out.append("    }\n");
        }
        return true;
    case RegInstructionKind::Intrinsic:
        if (ins.intrinsicName.rfind("indy:", 0) == 0) {
            // 自定义 bootstrap invokedynamic（阶段3）：站点元数据在 RegFunction::indySites，
            // 方法内的局部索引在文件级表中偏移 indySiteBase_。方法体只负责收集参数并调用
            // 文件级 bootstrap helper（finalize 生成），返回目标 MethodHandle 后
            // invokeWithArguments 完成实际调用。
            size_t localIdx = 0;
            try {
                localIdx = static_cast<size_t>(std::stoull(ins.intrinsicName.substr(5)));
            }
            catch (const std::exception&) {
                reason = "invalid indy site index in intrinsic name";
                return false;
            }
            if (localIdx >= fn.indySites.size()) {
                reason = "indy site index out of range";
                return false;
            }
            return emitInvokeDynamic(
                out, ins, fn.indySites[localIdx], indySiteBase + localIdx, reason);
        }
        if (ins.intrinsicName == "array.length") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "array.length",
                                          reason))
                return false;
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = _mj2x_regc_array_length(");
            out.append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]));
            out.append(");\n");
            return true;
        }
        if (ins.intrinsicName.rfind("instanceof:", 0) == 0) {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "instanceof",
                                          reason))
                return false;
            const ca::str::Utf8String cacheName = addClassCache(
                state, ca::str::Utf8StringRef::from_string_view(ins.intrinsicName.substr(11)));
            const std::string destName = regName(ins.dest.id);
            out.append("    ");
            out.append(destName.data(), destName.size());
            out.append(" = _mj2x_regc_instanceof_cached(");
            out.append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]));
            out.append(", &");
            out.append(cacheName);
            out.append(");\n");
            return true;
        }
        if (ins.intrinsicName == "monitor.enter") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.enter",
                                          reason))
                return false;
            out.append("    _mj2x_regc_monitor_enter(");
            out.append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]));
            out.append(");\n");
            return true;
        }
        if (ins.intrinsicName == "monitor.exit") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.exit",
                                          reason))
                return false;
            out.append("    _mj2x_regc_monitor_exit(");
            out.append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]));
            out.append(");\n");
            return true;
        }
        // JVM 的 LCMP/FCMPG/FCMPL/DCMPG/DCMPL 三值比较（-1/0/1），由 ssa_builder 的
        // lowerCmp 翻译为 cmp.nan_greater（FCMPG/DCMPG）或 cmp.nan_less（LCMP/FCMPL/DCMPL）。
        // 操作数类型来自 args[0]：浮点（F32/F64）需 NaN-aware 语义，整型（I64，LCMP）无 NaN。
        if (ins.intrinsicName == "cmp.nan_greater" || ins.intrinsicName == "cmp.nan_less") {
            if (ins.args.size() < 2) {
                reason = "cmp.nan_* intrinsic requires two operands";
                return false;
            }
            const ca::str::Utf8String lhs = operandExpr(ins.args[0]);
            const ca::str::Utf8String rhs = operandExpr(ins.args[1]);
            const SSAType             type = ins.args[0].type;
            const bool nanGreater = ins.intrinsicName == "cmp.nan_greater";
            // (lhs > rhs) - (lhs < rhs) 产出三值 -1/0/1，两种类型通用。
            CSourceStream threeWayBuilder;
            threeWayBuilder.append("((");
            threeWayBuilder.append(lhs);
            threeWayBuilder.append(" > ");
            threeWayBuilder.append(rhs);
            threeWayBuilder.append(") - (");
            threeWayBuilder.append(lhs);
            threeWayBuilder.append(" < ");
            threeWayBuilder.append(rhs);
            threeWayBuilder.append("))");
            const ca::str::Utf8String threeWay = threeWayBuilder.build();
            const std::string         destName = regName(ins.dest.id);
            if (ssaIsFloat(type)) {
                // 浮点：任一为 NaN 时按 nanGreater 取 1（dcmpg/fcmpg）或 -1（dcmpl/fcmpl）。
                const char* nanValue = nanGreater ? "1" : "-1";
                out.append("    ");
                out.append(destName.data(), destName.size());
                out.append(" = (isnan(");
                out.append(lhs);
                out.append(") || isnan(");
                out.append(rhs);
                out.append(")) ? ");
                out.append(nanValue);
                out.append(" : ");
                out.append(threeWay);
                out.append(";\n");
            }
            else {
                // 整型（LCMP）：long 不可能为 NaN，直接三值比较。
                out.append("    ");
                out.append(destName.data(), destName.size());
                out.append(" = ");
                out.append(threeWay);
                out.append(";\n");
            }
            return true;
        }
        reason = "unsupported RegC intrinsic: " + ins.intrinsicName;
        return false;
    case RegInstructionKind::Barrier:
        out.append("    mj2x_regc_barrier(_mj2x_regc_current_context(), ");
        out.append(cStringLiteral(ins.barrierReason));
        out.append(");\n");
        return true;
    default: break;
    }

    reason = "unreachable RegC instruction path";
    return false;
}

bool emitTerminator(CSourceStream& out, const RegTerminator& terminator,
                    const RegBlock* block, SSAType returnType,
                    const std::vector<const RegExceptionRegion*>& activeRegions,
                    RegCFunctionEmitState& state, int& dispatchIndex, int& edgeTempIndex,
                    std::string& reason)
{
    return std::visit(
        [&](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                out.append("    {\n");
                if (!emitEdgeCopies(out, t.target.copies, state, edgeTempIndex, reason))
                    return false;
                out.append("    goto block_b");
                out.append(std::to_string(t.target.block->id.value));
                out.append(";\n");
                out.append("    }\n");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.condition, "branch condition", reason))
                    return false;
                out.append("    if (");
                out.append(operandExpr(t.condition));
                out.append(") {\n");
                if (!emitEdgeCopies(out, t.trueTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out.append("        goto block_b");
                out.append(std::to_string(t.trueTarget.block->id.value));
                out.append(";\n");
                out.append("    } else {\n");
                if (!emitEdgeCopies(out, t.falseTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out.append("        goto block_b");
                out.append(std::to_string(t.falseTarget.block->id.value));
                out.append(";\n");
                out.append("    }\n");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "switch value", reason)) return false;
                out.append("    switch (");
                out.append(operandExpr(t.value));
                out.append(") {\n");
                for (const auto& c : t.cases) {
                    out.append("    case ");
                    out.append(std::to_string(c.key));
                    out.append(": {\n");
                    if (!emitEdgeCopies(out, c.target.copies, state, edgeTempIndex, reason))
                        return false;
                    out.append("        goto block_b");
                    out.append(std::to_string(c.target.block->id.value));
                    out.append(";\n");
                    out.append("    }\n");
                }
                out.append("    default: {\n");
                if (!emitEdgeCopies(out, t.defaultTarget.copies, state, edgeTempIndex, reason))
                    return false;
                out.append("        goto block_b");
                out.append(std::to_string(t.defaultTarget.block->id.value));
                out.append(";\n");
                out.append("    }\n");
                out.append("    }\n");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (t.hasValue) {
                    const RegCNativeObjectState* objectState = nativeObjectFor(state, t.value);
                    if (objectState != nullptr && isMutableStringKind(objectState->kind)) {
                        ca::str::Utf8String materializedExpression;
                        emitMaterializedStringBuilder(out, state, t.value, materializedExpression);
                        if (!emitExceptionDispatch(out, activeRegions, block, returnType, state,
                                                   dispatchIndex, edgeTempIndex, reason))
                            return false;
                        out.append("    _mj2x_regc_result = ");
                        out.append(materializedExpression);
                        out.append(";\n");
                    }
                    else {
                        if (objectState != nullptr) {
                            reason = "native object escaped through method return";
                            return false;
                        }
                        out.append("    _mj2x_regc_result = ");
                        out.append(operandExpr(t.value));
                        out.append(";\n");
                    }
                }
                out.append("    goto _mj2x_regc_cleanup;\n");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "throw", reason)) return false;
                out.append("    _mj2x_regc_throw(");
                out.append(operandExpr(t.value));
                out.append(");\n");
                if (!emitExceptionDispatch(out, activeRegions, block, returnType, state,
                                           dispatchIndex, edgeTempIndex, reason))
                    return false;
                if (returnType != SSAType::VOID) {
                    out.append("    _mj2x_regc_result = ");
                    out.append(defaultValue(returnType));
                    out.append(";\n");
                }
                out.append("    goto _mj2x_regc_cleanup;\n");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                if (returnType != SSAType::VOID) {
                    out.append("    _mj2x_regc_result = ");
                    out.append(defaultValue(returnType));
                    out.append(";\n");
                }
                out.append("    goto _mj2x_regc_cleanup;\n");
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

std::string poolCacheKey(const ca::str::Utf8StringRef& first, const ca::str::Utf8StringRef& second,
                         const ca::str::Utf8StringRef& third)
{
    return first.to_std_string() + "\n" + second.to_std_string() + "\n" + third.to_std_string();
}

}   // namespace

ca::str::Utf8String RegCCachePool::staticMethodCache(const ca::str::Utf8StringRef& owner,
                                                      const ca::str::Utf8StringRef& name,
                                                      const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticMethodCacheNames_.find(key);
    if (it != staticMethodCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_cache_tu_");
    cacheNameBuilder.append(std::to_string(staticMethodCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: static ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(owner));
    decl.append(", ");
    decl.append(cStringLiteral(name));
    decl.append(", ");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    staticMethodCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::instanceMethodCache(const ca::str::Utf8StringRef& owner,
                                                       const ca::str::Utf8StringRef& name,
                                                       const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceMethodCacheNames_.find(key);
    if (it != instanceMethodCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_instance_cache_tu_");
    cacheNameBuilder.append(std::to_string(instanceMethodCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: instance ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_instance_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(owner));
    decl.append(", ");
    decl.append(cStringLiteral(name));
    decl.append(", ");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL, NULL, NULL, NULL};\n");
    declarations_.push_back(decl.build());
    instanceMethodCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::resolvedCallSite(const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = resolvedCallSiteNames_.find(key);
    if (it != resolvedCallSiteNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_resolved_site_tu_");
    cacheNameBuilder.append(std::to_string(resolvedCallSiteIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: resolved call site ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_resolved_call_site_t ");
    decl.append(cacheName);
    decl.append(" = {NULL, NULL, NULL, NULL};\n");
    declarations_.push_back(decl.build());
    resolvedCallSiteNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::staticFieldCache(const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = staticFieldCacheNames_.find(key);
    if (it != staticFieldCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_field_cache_tu_");
    cacheNameBuilder.append(std::to_string(staticFieldCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: static field ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(owner));
    decl.append(", ");
    decl.append(cStringLiteral(name));
    decl.append(", ");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    staticFieldCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::instanceFieldCache(const ca::str::Utf8StringRef& owner,
                                                      const ca::str::Utf8StringRef& name,
                                                      const ca::str::Utf8StringRef& desc)
{
    const std::string key = poolCacheKey(owner, name, desc);
    auto              it  = instanceFieldCacheNames_.find(key);
    if (it != instanceFieldCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_instance_field_cache_tu_");
    cacheNameBuilder.append(std::to_string(instanceFieldCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: instance field ");
        decl.append(codegen::escapeCommentIdent(owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_instance_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(owner));
    decl.append(", ");
    decl.append(cStringLiteral(name));
    decl.append(", ");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL, NULL};\n");
    declarations_.push_back(decl.build());
    instanceFieldCacheNames_.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String RegCCachePool::classCache(const ca::str::Utf8StringRef& desc)
{
    auto       it  = classCacheNames_.find(desc.to_std_string());
    if (it != classCacheNames_.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_class_cache_tu_");
    cacheNameBuilder.append(std::to_string(classCacheIndex_++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (commentLevel_ >= RegCCommentLevel::Summary) {
        decl.append("// cache: class ");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_class_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL};\n");
    declarations_.push_back(decl.build());
    classCacheNames_.emplace(desc.to_std_string(), cacheName.clone());
    return cacheName;
}

std::string RegCCachePool::renderDeclarations() const
{
    CSourceStream out;
    for (const auto& declaration : declarations_) {
        out.append(declaration);
    }
    if (!declarations_.empty()) out.append("\n");
    return out.build().to_std_string();
}

RegCCachePool::RegCCachePool(RegCCommentLevel commentLevel)
    : commentLevel_(commentLevel)
{}

bool RegCCachePool::empty() const
{
    return declarations_.empty();
}

void emitRegCHelperPrelude(ca::str::Utf8StringBuilder& out)
{
    emitHelperPrelude(out);
}

RegCEmitter::RegCEmitter(RegCCachePool* cachePool, RegCCommentLevel commentLevel,
                         std::vector<RegFunction::InvokeDynamicSite>* fileIndySites,
                         size_t indySiteBase, bool traceCallLog, CompilerIndex* classIndex,
                         const std::unordered_map<std::string, std::string>* convertedFunctions,
                         const std::unordered_set<std::string>* indyFunctions,
                         bool resolvedDispatch)
    : cachePool_(cachePool)
    , commentLevel_(commentLevel)
    , fileIndySites_(fileIndySites)
    , indySiteBase_(indySiteBase)
    , traceCallLog_(traceCallLog)
    , classIndex_(classIndex)
    , convertedFunctions_(convertedFunctions)
    , indyFunctions_(indyFunctions)
    , resolvedDispatch_(resolvedDispatch)
{}

std::string RegCEmitter::functionNameFor(const SSAFunctionSymbol& symbol)
{
    return functionName(symbol);
}

RegCEmitResult RegCEmitter::emitFunction(const RegFunction& fn)
{
    RegCEmitResult result;
    result.functionName = functionName(fn.symbol);
    if (!fn.entry) {
        result.reason = "RegC requires an entry block";
        return result;
    }
    if (fileIndySites_ != nullptr && !fn.indySites.empty()) {
        fileIndySites_->insert(fileIndySites_->end(), fn.indySites.begin(), fn.indySites.end());
    }

    std::string validationReason;
    if (!validateRegIRNoStackSlots(fn, validationReason)) {
        result.reason = validationReason;
        return result;
    }

    std::map<uint32_t, SSAType> regs;
    if (!collectRegisters(fn, regs, result.reason)) return result;
    const RegCStringBuilderPlan stringBuilderPlan      = analyzeStringBuilders(fn);
    const std::set<uint32_t>    nativeWrapperRegisters = analyzeNativeWrappers(fn);

    RegCFunctionEmitState emitState;
    emitState.functionName                       = result.functionName;
    emitState.symbol                             = fn.symbol;
    emitState.cachePool                          = cachePool_;
    emitState.commentLevel                       = commentLevel_;
    emitState.traceCalls                         = traceCallLog_;
    emitState.classIndex                         = classIndex_;
    emitState.convertedFunctions                 = convertedFunctions_;
    emitState.indyFunctions                      = indyFunctions_;
    emitState.resolvedDispatch                   = resolvedDispatch_;
    emitState.nativeStringBuilderRegisters       = stringBuilderPlan.nativeRegisters;
    emitState.nativeStringBuilderOwners          = stringBuilderPlan.ownerRegisters;
    emitState.nativeStringBuilderOwnerByRegister = stringBuilderPlan.ownerByRegister;
    emitState.nativeWrapperRegisters             = nativeWrapperRegisters;
    for (uint32_t id : stringBuilderPlan.nativeRegisters) {
        const auto kindIt = stringBuilderPlan.kindByRegister.find(id);
        if (kindIt == stringBuilderPlan.kindByRegister.end()) {
            result.reason = "mutable string plan is missing register kind";
            return result;
        }
        emitState.nativeObjects.emplace(id,
                                        RegCNativeObjectState{kindIt->second, regName(RegId{id})});
    }
    int                dispatchIndex = 0;
    int                edgeTempIndex = 0;
    CSourceStream body;
    for (const auto& block : fn.blocks) {
        auto activeRegions = activeExceptionRegions(fn, block.get());
        if (commentLevel_ >= RegCCommentLevel::Summary) {
            body.append("// block_b");
            body.append(std::to_string(block->id.value));
            body.append(": ");
            if (block.get() == fn.entry) {
                body.append("entry");
            }
            else {
                const RegExceptionRegion* handlerRegion = nullptr;
                for (const auto& region : fn.exceptionRegions) {
                    if (region.handler == block.get()) {
                        handlerRegion = &region;
                        break;
                    }
                }
                if (handlerRegion != nullptr) {
                    body.append("exception handler of ");
                    body.append(codegen::escapeCommentIdent(
                        ca::str::Utf8StringRef::from_string_view(handlerRegion->caughtType)));
                }
                else {
                    body.append("block");
                }
            }
            body.append("\n");
        }
        body.append("block_b");
        body.append(std::to_string(block->id.value));
        body.append(":\n");
        for (const auto& ins : block->instructions) {
            if (commentLevel_ >= RegCCommentLevel::Detailed) {
                body.append("// ");
                body.append(regInstructionLine(ins));
                body.append("\n");
            }
            if (!emitInstruction(body, ins, emitState, fn, indySiteBase_, result.reason)) return result;
            if (ins.effect.mayThrow) {
                if (!emitExceptionDispatch(body,
                                           activeRegions,
                                           block.get(),
                                           fn.symbol.returnType,
                                           emitState,
                                           dispatchIndex,
                                           edgeTempIndex,
                                           result.reason))
                    return result;
            }
        }
        if (commentLevel_ >= RegCCommentLevel::Detailed) {
            const ca::str::Utf8String termLines = regTerminatorLines(*block);
            std::string_view          sv(termLines);
            // regTerminatorLines 每行以 '\n' 结尾；逐行加 "// " 前缀作为注释，避免裸 IR 文本进入 C 流。
            size_t pos = 0;
            while (pos < sv.size()) {
                size_t nl = sv.find('\n', pos);
                if (nl == std::string_view::npos) nl = sv.size();
                body.append("// ");
                body.append(sv.substr(pos, nl - pos));
                body.append("\n");
                pos = nl + 1;
            }
        }
        if (!emitTerminator(body,
                            block->terminator,
                            block.get(),
                            fn.symbol.returnType,
                            activeRegions,
                            emitState,
                            dispatchIndex,
                            edgeTempIndex,
                            result.reason)) {
            return result;
        }
        body.append("\n");
    }

    CSourceStream out;
    for (const auto& declaration : emitState.staticMethodCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.staticFieldCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.classCacheDeclarations) {
        out.append(declaration);
    }
    if (!emitState.staticMethodCacheDeclarations.empty() ||
        !emitState.staticFieldCacheDeclarations.empty() ||
        !emitState.classCacheDeclarations.empty())
        out.append("\n");

    if (commentLevel_ >= RegCCommentLevel::Summary) {
        out.append("// RegC method: ");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.owner)));
        out.append(".");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.name)));
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.desc)));
        out.append("\n");
    }

    out.append("static ");
    out.append(cType(fn.symbol.returnType));
    out.append(" ");
    out.append(result.functionName);
    out.append("(");
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out.append(", ");
        out.append(cType(fn.parameters[i].type));
        out.append(" ");
        {
            const std::string paramName = regName(fn.parameters[i].id);
            out.append(paramName.data(), paramName.size());
        }
    }
    out.append(") {\n");

    if (fn.symbol.returnType != SSAType::VOID) {
        out.append("    ");
        out.append(cType(fn.symbol.returnType));
        out.append(" _mj2x_regc_result = ");
        out.append(defaultValue(fn.symbol.returnType));
        out.append(";\n");
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
            const std::string localRegName = regName(RegId{id});
            out.append("    ");
            out.append(cType(type));
            out.append(" ");
            out.append(localRegName.data(), localRegName.size());
            out.append(" = 0;\n");
        }
    }
    // Declare edge-copy temporaries based on the types actually captured during emit
    // (emitState.edgeTempTypes). This mirrors the emit path exactly, including the
    // exception-dispatch re-emission that a static CFG walk (collectEdgeCopyTempTypes)
    // would undercount when a protected block has multiple mayThrow instructions.
    for (size_t i = 0; i < emitState.edgeTempTypes.size(); ++i) {
        out.append("    ");
        out.append(cType(emitState.edgeTempTypes[i]));
        out.append(" edge_tmp_");
        out.append(std::to_string(i));
        out.append(" = 0;\n");
    }
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto objectIt = emitState.nativeObjects.find(owner);
        if (objectIt == emitState.nativeObjects.end()) {
            result.reason = "mutable string owner is missing native object state";
            return result;
        }
        out.append("    ");
        out.append(nativeObjectType(objectIt->second.kind));
        out.append("* ");
        out.append(stringBuilderOwnerName(RegId{owner}));
        out.append(" = NULL;\n");
        out.append("    mj2x_ref ");
        out.append(stringBuilderMaterializedName(RegId{owner}));
        out.append(" = NULL;\n");
    }
    if (!regs.empty()) out.append("\n");

    // R6 前置：JNI local ref 引用管理。RegC 每个 native 方法在单一 JNI 帧内运行，
    // 产 object 的 helper（call_ref / class_literal / alloc / ref-field load 等）返回的
    // local ref 全程累积不释放，反射密集方法会撑爆 JNI 默认保证的 16 槽 local ref 表。
    // 用 PushLocalFrame(入口) / PopLocalFrame(单出口，promote 返回值) 批量释放，语义等价
    // StackC 的 trackLocalRef/clear，但零 C 数组、无 c2j stack anchor。缓存 class/method
    // 走 NewGlobalRef，不受帧影响。纯标量方法（无产 ref 操作）不发射，保留「零 JNI」形态。
    const ca::str::Utf8String bodySource = body.build();
    const bool                needsLocalFrame = bodyProducesLocalRefs(bodySource);
    if (needsLocalFrame) {
        out.append("    if ((*_mj2x_regc_env)->PushLocalFrame(_mj2x_regc_env, 16) != 0) {\n");
        if (fn.symbol.returnType == SSAType::VOID)
            out.append("        return;\n");
        else
            out.append("        return _mj2x_regc_result;\n");
        out.append("    }\n");
    }
    out.append("    goto block_b");
    out.append(std::to_string(fn.entry->id.value));
    out.append(";\n\n");
    out.append(bodySource);

    if (commentLevel_ >= RegCCommentLevel::Summary) {
        out.append("// _mj2x_regc_cleanup: native object teardown + local frame pop\n");
    }
    out.append("_mj2x_regc_cleanup:\n");
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto  objectIt = emitState.nativeObjects.find(owner);
        const char* destroySymbol =
            objectIt != emitState.nativeObjects.end() &&
                    objectIt->second.kind == RegCIntrinsicNativeObjectKind::StringBuffer
                ? "mj2x_string_buffer_destroy"
                : "mj2x_string_builder_destroy";
        out.append("    ");
        out.append(destroySymbol);
        out.append("(");
        out.append(stringBuilderOwnerName(RegId{owner}));
        out.append(");\n");
    }
    // PopLocalFrame 释放本帧全部 local ref；REF 返回值通过 result 参数 promote 到父帧，
    // 非 REF/VOID 传 NULL。PopLocalFrame 对 pending exception 安全（返回值可能为 NULL）。
    if (needsLocalFrame) {
        if (fn.symbol.returnType == SSAType::REF) {
            out.append("    _mj2x_regc_result = (mj2x_ref)(*_mj2x_regc_env)->PopLocalFrame("
                       "_mj2x_regc_env, (jobject)_mj2x_regc_result);\n");
        }
        else {
            out.append("    (*_mj2x_regc_env)->PopLocalFrame(_mj2x_regc_env, NULL);\n");
        }
    }
    if (fn.symbol.returnType == SSAType::VOID)
        out.append("    return;\n");
    else
        out.append("    return _mj2x_regc_result;\n");

    out.append("}\n");

    const std::string sourceCode = out.build().to_std_string();
    result.source = sourceCode;
    if (!validateRegCNoStackAnchors(result.source, result.reason)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.reason.clear();
    return result;
}

bool emitInvokeDynamic(CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       std::string& reason)
{
    const ca::str::Utf8StringRef descRef = ca::str::Utf8StringRef::from_cstr(site.desc.c_str());
    auto                         argTypes = Type::getArgumentTypes(descRef);
    const int                    argCount = static_cast<int>(argTypes.size());
    if (argCount != static_cast<int>(ins.args.size())) {
        reason = "indy arg count mismatch";
        return false;
    }
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();

    out.append("    {\n");
    out.append("        jclass _mj2x_indy_obj_cls = (jclass)_mj2x_regc_class_literal("
               "_mj2x_regc_current_context(), \"java/lang/Object\");\n");
    out.append("        jobjectArray _mj2x_indy_args = (jobjectArray)(*_mj2x_regc_env)"
               "->NewObjectArray(_mj2x_regc_env, ");
    out.append(std::to_string(argCount));
    out.append(", _mj2x_indy_obj_cls, NULL);\n");
    out.append("        if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }\n");
    for (int i = 0; i < argCount; ++i) {
        const ca::str::Utf8String expr = operandExpr(ins.args[i]);
        std::string               boxExpr;
        switch (ins.args[i].type) {
        case SSAType::REF:
            boxExpr = codegen::exprCast("jobject", codegen::exprParen(expr.to_std_string()));
            break;
        case SSAType::BOOL:
        case SSAType::I8:
        case SSAType::I16:
        case SSAType::CHAR:
        case SSAType::I32:
            boxExpr = codegen::exprCall("_mj2x_regc_box_i32",
                                        {codegen::exprCast("int32_t",
                                                           codegen::exprParen(expr.to_std_string()))});
            break;
        case SSAType::I64:
            boxExpr = codegen::exprCall("_mj2x_regc_box_i64",
                                        {codegen::exprCast("int64_t",
                                                           codegen::exprParen(expr.to_std_string()))});
            break;
        case SSAType::F32:
            boxExpr = codegen::exprCall("_mj2x_regc_box_f32",
                                        {codegen::exprCast("float",
                                                           codegen::exprParen(expr.to_std_string()))});
            break;
        case SSAType::F64:
            boxExpr = codegen::exprCall("_mj2x_regc_box_f64",
                                        {codegen::exprCast("double",
                                                           codegen::exprParen(expr.to_std_string()))});
            break;
        default:
            reason = "unsupported indy argument type";
            return false;
        }
        out.append("        ");
        out.append(codegen::exprCall("(*_mj2x_regc_env)->SetObjectArrayElement",
                                     {"_mj2x_regc_env", "_mj2x_indy_args", std::to_string(i),
                                      boxExpr}));
        out.append(";\n");
    }
    out.append("        jobject _mj2x_indy_target = _mj2x_regc_indy_bsm_");
    out.append(std::to_string(globalSiteIndex));
    out.append("(_mj2x_regc_owner_class);\n");
    out.append("        if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }\n");
    out.append("        if (_mj2x_indy_target == NULL) { goto _mj2x_regc_cleanup; }\n");
    out.append("        jclass _mj2x_indy_mh_cls = (jclass)_mj2x_regc_class_literal("
               "_mj2x_regc_current_context(), \"java/lang/invoke/MethodHandle\");\n");
    out.append("        jmethodID _mj2x_indy_invoke_mid = (*_mj2x_regc_env)->GetMethodID("
               "_mj2x_regc_env, _mj2x_indy_mh_cls, \"invokeWithArguments\", "
               "\"([Ljava/lang/Object;)Ljava/lang/Object;\");\n");
    out.append("        if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }\n");
    out.append("        jobject _mj2x_indy_result = (jobject)(*_mj2x_regc_env)->CallObjectMethod("
               "_mj2x_regc_env, _mj2x_indy_target, _mj2x_indy_invoke_mid, _mj2x_indy_args);\n");
    out.append("        if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }\n");
    if (ins.hasDest) {
        out.append("        ");
        out.append(destName.data(), destName.size());
        out.append(" = ");
        std::string resultExpr;
        switch (ins.dest.type) {
        case SSAType::REF:
            resultExpr = codegen::exprCast("mj2x_ref", "_mj2x_indy_result");
            break;
        case SSAType::BOOL:
        case SSAType::I8:
        case SSAType::I16:
        case SSAType::CHAR:
        case SSAType::I32:
            resultExpr = codegen::exprCall("_mj2x_regc_unbox_i32", {"_mj2x_indy_result"});
            break;
        case SSAType::I64:
            resultExpr = codegen::exprCall("_mj2x_regc_unbox_i64", {"_mj2x_indy_result"});
            break;
        case SSAType::F32:
            resultExpr = codegen::exprCall("_mj2x_regc_unbox_f32", {"_mj2x_indy_result"});
            break;
        case SSAType::F64:
            resultExpr = codegen::exprCall("_mj2x_regc_unbox_f64", {"_mj2x_indy_result"});
            break;
        default:
            reason = "unsupported indy result type";
            return false;
        }
        out.append(resultExpr);
        out.append(";\n");
    }
    else {
        out.append("        (void)_mj2x_indy_result;\n");
    }
    out.append("    }\n");
    return true;
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

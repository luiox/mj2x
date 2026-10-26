/// @file regc_expr.cpp
/// @brief RegC 发射器表达式/类型/命名 helper（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// 纯函数级 helper：寄存器/函数命名、C 类型映射、操作数表达式、描述符解析、
/// native 对象与 StringBuilder 名称/表达式、varargs 实参列表。无发射状态依赖，
/// 从原 regc_emitter.cpp 纯搬移，生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "libca/str/format.hpp"
#include "mj2x/codegen/c_helpers.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace mj2x {

using codegen::CSourceStream;
using codegen::cStringLiteral;
using codegen::utf8ToModifiedUtf8;

namespace {

/// 字面量加密登记池（--strings-encrypt）：发射期间由 RegCStringEncryptScope
/// 安装（单线程发射，无并发）；空 = 未启用，ldc_string 保持明文发射。
RegCCachePool* g_stringEncryptPool = nullptr;

}   // namespace

RegCCachePool* currentStringEncryptPool()
{
    return g_stringEncryptPool;
}

void setCurrentStringEncryptPool(RegCCachePool* pool)
{
    g_stringEncryptPool = pool;
}

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
    return ca::str::format_std("r{}", id.value);
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
            // INT64_MIN 的十进制字面量 -9223372036854775808LL 超出 long long
            // 可表示域（标准 C ill-formed，MSVC/GCC 宽限为告警），改发射等价
            // 减法表达式。
            if (operand.intValue == INT64_MIN) {
                os.append("(int64_t)(-9223372036854775807LL - 1)");
            }
            else {
                os.append("(int64_t)");
                os.append(ca::str::format_std("{}", operand.intValue));
                os.append("LL");
            }
        }
        else {
            os.append(ca::str::format_std("{}", operand.intValue));
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
            const auto content = utf8ToModifiedUtf8(
                operand.tag.substr(std::string("ldc_string:").size()));
            CSourceStream os;
            os.append("_mj2x_intrinsic_string_literal(_mj2x_regc_current_context(), ");
            if (RegCCachePool* encryptPool = currentStringEncryptPool()) {
                // --strings-encrypt：字面量以密文缓冲引用（JNI_OnLoad 批量解密，
                // C 源 at-rest 不含明文）；缓冲按内容哈希去重，多调用点共享同一
                // 指针，_mj2x_intrinsic_string_literal 的指针等值快路径照常命中。
                os.append(encryptPool->encryptedLiteralBuffer(content));
            }
            else {
                os.append(cStringLiteral(content));
            }
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
        // 其余 opaque：REF 槽位发 NULL（指针零值），标量/UNKNOWN 槽位发 0。
        // 标量槽位若发 NULL，边拷贝形态 `(int32_t)(NULL)` 会触发 MSVC C4311
        // 指针截断警告；undef 值仅用于满足 SSA 边拷贝，标量 0 语义等价。
        CSourceStream os;
        os.append(operand.type == SSAType::REF ? "NULL" : "0");
        os.append(" /* opaque:");
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
    return ca::str::format("native_owner_r{}", id.value);
}

ca::str::Utf8String stringBuilderMaterializedName(RegId id)
{
    return ca::str::format("native_materialized_r{}", id.value);
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

ca::str::Utf8String callArgs(const std::vector<RegOperand>& args)
{
    CSourceStream os;
    os.append(ca::str::format_std("{}", args.size()));
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
    os.append(ca::str::format_std("{}", args.size()));
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

}   // namespace mj2x

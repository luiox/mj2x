#include "stackc_backend.hpp"
#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/code_snippet.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <string_view>
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using codegen::CSourceStream;

// label 标识：LabelNode* 转 int（与 code_converter 的 labelKey 一致；见 dev_plan R1a.3）。
static int labelKey(jbytecode::core::Label* lbl)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

// label 的 C 标识符：用 uintptr_t 的十六进制，保证永远是合法 C 标识符（0-9a-f），
// 不会出现负号（int 强转可能为负，label_-12345 不是合法 C 标识符）。
static std::string labelIdent(jbytecode::core::Label* lbl)
{
    std::ostringstream os;
    os << "L" << std::hex << reinterpret_cast<uintptr_t>(lbl);
    return os.str();
}

// ============================================================
// JNI name mangling + C string escaping
// ------------------------------------------------------------
// Stateless and shared with RegC: the single home is now
// mj2x/codegen/c_helpers.hpp (codegen::mangleName / mangleMethod /
// cStringLiteral).  This file just pulls them in via a namespace alias.
// ============================================================

namespace {
using codegen::cStringLiteral;
using codegen::SnippetVars;
}   // namespace

enum class StackCSnippet
{
    PushStackValue,
    LocalLoad,
    LocalStore,
    BinaryUnsignedWrap,
    ReturnVoid,
    NullPointerTrap,
    PendingExceptionCheck,
    ArrayLength,
    ObjectArrayLoad,
    ObjectArrayStore,
    PrimitiveArrayLoad,
    PrimitiveArrayStore,
};

static const codegen::CodeSnippetSet<StackCSnippet>& stackCSnippets()
{
    static const codegen::CodeSnippetSet<StackCSnippet> snippets({
        {StackCSnippet::PushStackValue, "    stack[++sp]${acc} = ${value};\n"},
        {StackCSnippet::LocalLoad, "    stack[++sp]${acc} = locals[${slot}]${acc};\n"},
        {StackCSnippet::LocalStore, "    locals[${slot}]${acc} = stack[sp--]${acc};\n"},
        {StackCSnippet::BinaryUnsignedWrap,
         "    ${lhs}${acc} = (${signed_type})((${unsigned_type})${lhs}${acc} ${op} "
         "(${unsigned_type})${rhs}${acc});\n"
         "    sp--;\n"},
        {StackCSnippet::ReturnVoid,
         "    _mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, NULL);\n"
         "    return;\n"},
        {StackCSnippet::NullPointerTrap,
         "    if (${ref} == NULL) {\n"
         "        jclass ex_class = (jclass)${ex_class};\n"
         "        (*env)->ThrowNew(env, ex_class, \"null reference\");\n"
         "${dispatch}"
         "    }\n"},
        {StackCSnippet::PendingExceptionCheck,
         "${pad}if ((*env)->ExceptionCheck(env)) {\n"
         "${dispatch}"
         "${pad}}\n"},
        {StackCSnippet::ArrayLength,
         "    stack[sp].i = (*env)->GetArrayLength(env, stack[sp].l);\n"},
        {StackCSnippet::ObjectArrayLoad,
         "    stack[sp - 1].l = ${value};\n"
         "    sp--;\n"},
        {StackCSnippet::ObjectArrayStore,
         "    (*env)->SetObjectArrayElement(env, (jobjectArray)stack[sp - 2].l, "
         "stack[sp - 1].i, stack[sp].l);\n"
         "    sp -= 3;\n"},
        {StackCSnippet::PrimitiveArrayLoad,
         "    {\n"
         "        ${type} value;\n"
         "        (*env)->Get${suffix}ArrayRegion(env, (${array_type})stack[sp - 1].l, "
         "stack[sp].i, 1, &value);\n"
         "        stack[sp - 1]${acc} = (${target_type})value;\n"
         "    }\n"
         "    sp--;\n"},
        {StackCSnippet::PrimitiveArrayStore,
         "    {\n"
         "        ${type} value = (${type})stack[sp]${acc};\n"
         "        (*env)->Set${suffix}ArrayRegion(env, (${array_type})stack[sp - 2].l, "
         "stack[sp - 1].i, 1, &value);\n"
         "    }\n"
         "    sp -= 3;\n"},
    });
    return snippets;
}

static std::string renderStackCSnippet(StackCSnippet key, const SnippetVars& vars)
{
    return stackCSnippets().render(key, vars);
}

static void emitCLine(std::ostringstream& out, int indentLevel, const std::string& line)
{
    CSourceStream src;
    for (int i = 0; i < indentLevel; ++i) src.indent();
    src.emitLine(line);
    out << src.str();
}

static std::uint64_t fnv1a64(std::string_view value, std::uint64_t seed = 1469598103934665603ULL)
{
    std::uint64_t hash = seed;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string hex64(std::uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << value;
    return os.str();
}

static std::string nativeFunctionName(const std::string& className, const std::string& methodName,
                                      const std::string& methodDesc)
{
    auto hash = fnv1a64(className);
    hash      = fnv1a64(methodName, hash);
    hash      = fnv1a64(methodDesc, hash);
    return "__mj2x_c_" + hex64(hash);
}

static std::string registerFunctionName(const std::string& className)
{
    std::string ident = "_mj2x_register_";
    for (unsigned char c : className) {
        if (std::isalnum(c)) {
            ident += static_cast<char>(c);
        }
        else {
            ident += "_";
            ident += hex64(c).substr(14);
        }
    }
    return ident;
}

static std::string cacheIdentifier(const char* prefix, const std::string& key)
{
    return std::string(prefix) + "_" + hex64(fnv1a64(key));
}

static bool isCategory2Kind(PrimitiveKind kind)
{
    return kind == PrimitiveKind::LONG || kind == PrimitiveKind::DOUBLE;
}

static bool stackOpTopIsCategory2(const MIRStackOp* op, size_t depth = 0)
{
    return op != nullptr && depth < op->stackBeforeTop.size() &&
           isCategory2Kind(op->stackBeforeTop[depth]);
}

// ============================================================
// Type mapping helpers
// ============================================================

static const char* primitiveToJNIType(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "jboolean";
    case PrimitiveKind::BYTE: return "jbyte";
    case PrimitiveKind::CHAR: return "jchar";
    case PrimitiveKind::SHORT: return "jshort";
    case PrimitiveKind::INT: return "jint";
    case PrimitiveKind::LONG: return "jlong";
    case PrimitiveKind::FLOAT: return "jfloat";
    case PrimitiveKind::DOUBLE: return "jdouble";
    case PrimitiveKind::VOID: return "void";
    default: return "jobject";
    }
}

static const char* primitiveToJNISignature(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "Z";
    case PrimitiveKind::BYTE: return "B";
    case PrimitiveKind::CHAR: return "C";
    case PrimitiveKind::SHORT: return "S";
    case PrimitiveKind::INT: return "I";
    case PrimitiveKind::LONG: return "J";
    case PrimitiveKind::FLOAT: return "F";
    case PrimitiveKind::DOUBLE: return "D";
    case PrimitiveKind::VOID: return "V";
    default: return "Ljava/lang/Object;";
    }
}

static const char* primitiveToJNISuffix(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "Boolean";
    case PrimitiveKind::BYTE: return "Byte";
    case PrimitiveKind::CHAR: return "Char";
    case PrimitiveKind::SHORT: return "Short";
    case PrimitiveKind::INT: return "Int";
    case PrimitiveKind::LONG: return "Long";
    case PrimitiveKind::FLOAT: return "Float";
    case PrimitiveKind::DOUBLE: return "Double";
    default: return "Object";
    }
}

static const char* primitiveToJNIArrayType(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "jbooleanArray";
    case PrimitiveKind::BYTE: return "jbyteArray";
    case PrimitiveKind::CHAR: return "jcharArray";
    case PrimitiveKind::SHORT: return "jshortArray";
    case PrimitiveKind::INT: return "jintArray";
    case PrimitiveKind::LONG: return "jlongArray";
    case PrimitiveKind::FLOAT: return "jfloatArray";
    case PrimitiveKind::DOUBLE: return "jdoubleArray";
    default: return "jobjectArray";
    }
}

static const char* primitiveToNewArrayFn(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "NewBooleanArray";
    case PrimitiveKind::BYTE: return "NewByteArray";
    case PrimitiveKind::CHAR: return "NewCharArray";
    case PrimitiveKind::SHORT: return "NewShortArray";
    case PrimitiveKind::INT: return "NewIntArray";
    case PrimitiveKind::LONG: return "NewLongArray";
    case PrimitiveKind::FLOAT: return "NewFloatArray";
    case PrimitiveKind::DOUBLE: return "NewDoubleArray";
    default: return nullptr;
    }
}

static const char* stackAccessor(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::INT: return ".i";
    case PrimitiveKind::LONG: return ".j";
    case PrimitiveKind::FLOAT: return ".f";
    case PrimitiveKind::DOUBLE: return ".d";
    case PrimitiveKind::BOOLEAN: return ".z";
    case PrimitiveKind::BYTE: return ".b";
    case PrimitiveKind::CHAR: return ".c";
    case PrimitiveKind::SHORT: return ".s";
    default: return ".l";
    }
}

static bool narrowOnOperandStack(PrimitiveKind k)
{
    return k == PrimitiveKind::BOOLEAN || k == PrimitiveKind::BYTE || k == PrimitiveKind::CHAR ||
           k == PrimitiveKind::SHORT;
}

static const char* stackValueAccessor(PrimitiveKind k)
{
    return narrowOnOperandStack(k) ? ".i" : stackAccessor(k);
}

static const char* castResultAccessor(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BYTE:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::SHORT: return ".i";
    default: return stackAccessor(k);
    }
}

static std::string trackLocalRefExpr(const std::string& expr)
{
    return "_mj2x_track_local_ref(env, _mj2x_refs, &_mj2x_ref_count, " + expr + ")";
}

static std::string classExprForDescriptor(const std::string& typeDesc,
                                          const std::string& ownerExpr = "_mj2x_owner_class")
{
    if (typeDesc.size() >= 2 && typeDesc.front() == 'L' && typeDesc.back() == ';') {
        return "(jclass)" + trackLocalRefExpr("_mj2x_find_class(env, " + ownerExpr + ", \"" +
                                              typeDesc.substr(1, typeDesc.size() - 2) + "\")");
    }
    return "(jclass)" +
           trackLocalRefExpr("_mj2x_find_class(env, " + ownerExpr + ", \"" + typeDesc + "\")");
}

static const char* defaultReturnValue(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::VOID: return "";
    case PrimitiveKind::FLOAT: return "0.0f";
    case PrimitiveKind::DOUBLE: return "0.0";
    case PrimitiveKind::OBJECT: return "NULL";
    default: return "0";
    }
}

static int loadOpcodeForSort(int sort)
{
    switch (sort) {
    case Type::LONG: return Opcodes::LLOAD;
    case Type::FLOAT: return Opcodes::FLOAD;
    case Type::DOUBLE: return Opcodes::DLOAD;
    case Type::OBJECT:
    case Type::ARRAY: return Opcodes::ALOAD;
    default: return Opcodes::ILOAD;
    }
}

static int returnOpcodeForKind(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::VOID: return Opcodes::RETURN;
    case PrimitiveKind::LONG: return Opcodes::LRETURN;
    case PrimitiveKind::FLOAT: return Opcodes::FRETURN;
    case PrimitiveKind::DOUBLE: return Opcodes::DRETURN;
    case PrimitiveKind::OBJECT: return Opcodes::ARETURN;
    default: return Opcodes::IRETURN;
    }
}

static PrimitiveKind methodReturnKind(const std::string& methodDesc)
{
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(methodDesc.c_str()))
            .getDescriptor()
            .to_std_string());
}

static std::string prependReceiverToMethodDesc(const std::string& ownerClassName,
                                               const std::string& methodDesc)
{
    const auto close = methodDesc.find(')');
    if (methodDesc.empty() || methodDesc[0] != '(' || close == std::string::npos) return methodDesc;
    return "(L" + ownerClassName + ";" + methodDesc.substr(1);
}

static bool isInterfaceDefaultMethod(const CompilerClass& clazz, const MethodNode& method)
{
    if ((clazz.classNode->access & Opcodes::ACC_INTERFACE) == 0) return false;
    if ((method.access & (Opcodes::ACC_ABSTRACT | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE)) != 0)
        return false;
    const auto name = method.name.to_std_string();
    return name != "<init>" && name != "<clinit>";
}

static std::string utf8RefToString(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

static std::string typeDescriptorString(const Type& type)
{
    return type.getDescriptor().to_std_string();
}

static std::string methodTypeDescriptorString(const Type& type)
{
    return type.getSort() == Type::METHOD ? type.getDescriptor().to_std_string() : type.toString();
}

static const char* boxedClassName(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "java/lang/Boolean";
    case PrimitiveKind::BYTE: return "java/lang/Byte";
    case PrimitiveKind::CHAR: return "java/lang/Character";
    case PrimitiveKind::SHORT: return "java/lang/Short";
    case PrimitiveKind::INT: return "java/lang/Integer";
    case PrimitiveKind::LONG: return "java/lang/Long";
    case PrimitiveKind::FLOAT: return "java/lang/Float";
    case PrimitiveKind::DOUBLE: return "java/lang/Double";
    case PrimitiveKind::VOID: return "java/lang/Void";
    default: return "java/lang/Object";
    }
}

static const char* boxedValueOfDesc(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "(Z)Ljava/lang/Boolean;";
    case PrimitiveKind::BYTE: return "(B)Ljava/lang/Byte;";
    case PrimitiveKind::CHAR: return "(C)Ljava/lang/Character;";
    case PrimitiveKind::SHORT: return "(S)Ljava/lang/Short;";
    case PrimitiveKind::INT: return "(I)Ljava/lang/Integer;";
    case PrimitiveKind::LONG: return "(J)Ljava/lang/Long;";
    case PrimitiveKind::FLOAT: return "(F)Ljava/lang/Float;";
    case PrimitiveKind::DOUBLE: return "(D)Ljava/lang/Double;";
    default: return "(Ljava/lang/Object;)Ljava/lang/Object;";
    }
}

static const char* boxedUnboxMethod(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "booleanValue";
    case PrimitiveKind::BYTE: return "byteValue";
    case PrimitiveKind::CHAR: return "charValue";
    case PrimitiveKind::SHORT: return "shortValue";
    case PrimitiveKind::INT: return "intValue";
    case PrimitiveKind::LONG: return "longValue";
    case PrimitiveKind::FLOAT: return "floatValue";
    case PrimitiveKind::DOUBLE: return "doubleValue";
    default: return "";
    }
}

static const char* boxedUnboxDesc(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "()Z";
    case PrimitiveKind::BYTE: return "()B";
    case PrimitiveKind::CHAR: return "()C";
    case PrimitiveKind::SHORT: return "()S";
    case PrimitiveKind::INT: return "()I";
    case PrimitiveKind::LONG: return "()J";
    case PrimitiveKind::FLOAT: return "()F";
    case PrimitiveKind::DOUBLE: return "()D";
    default: return "()Ljava/lang/Object;";
    }
}

static void emitExceptionReturn(const std::string& methodDesc, std::ostringstream& out,
                                int indent = 8)
{
    auto retType = methodReturnKind(methodDesc);
    out << std::string(static_cast<size_t>(indent), ' ');
    out << "_mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, NULL);\n";
    out << std::string(static_cast<size_t>(indent), ' ');
    if (retType == PrimitiveKind::VOID)
        out << "return;\n";
    else
        out << "return " << defaultReturnValue(retType) << ";\n";
}

// ============================================================
// Binary op → C operator
// ============================================================

static const char* binaryOpToC(BinaryOp op)
{
    switch (op) {
    case BinaryOp::ADD: return "+";
    case BinaryOp::SUB: return "-";
    case BinaryOp::MUL: return "*";
    case BinaryOp::DIV: return "/";
    case BinaryOp::REM: return "%";
    case BinaryOp::SHL: return "<<";
    case BinaryOp::SHR: return ">>";
    case BinaryOp::USHR: return ">>";   // unsigned shift — same in C for unsigned
    case BinaryOp::AND: return "&";
    case BinaryOp::OR: return "|";
    case BinaryOp::XOR: return "^";
    default: return "+";
    }
}

static const char* cmpOpToC(CmpOp op)
{
    switch (op) {
    case CmpOp::EQ: return "==";
    case CmpOp::NE: return "!=";
    case CmpOp::LT: return "<";
    case CmpOp::GE: return ">=";
    case CmpOp::GT: return ">";
    case CmpOp::LE: return "<=";
    default: return "==";
    }
}

struct CExceptionHandlerInfo
{
    jbytecode::core::Label* start   = nullptr;
    jbytecode::core::Label* end     = nullptr;
    jbytecode::core::Label* handler = nullptr;
    std::string             catchType;
};

struct CExceptionDispatchInfo
{
    std::string      label;
    std::vector<int> handlerIndices;
};

struct CNativeRegistration
{
    std::string javaName;
    std::string javaDesc;
    std::string cFunctionName;
};

struct CHiddenClinitProxy
{
    std::string          ownerClassName;
    std::string          hiddenClassName;
    std::string          hiddenMethodName;
    std::string          hiddenMethodDesc;
    std::string          compiledFunctionName;
    std::string          wrapperFunctionName;
    std::string          callFunctionName;
    std::string          classBytesName;
    std::string          classRefName;
    std::string          methodIdName;
    std::string          doneFlagName;
    std::vector<uint8_t> classBytes;
};

struct CHiddenInterfaceDefaultProxy
{
    std::string          ownerClassName;
    std::string          javaName;
    std::string          javaDesc;
    std::string          hiddenClassName;
    std::string          hiddenMethodName;
    std::string          hiddenMethodDesc;
    std::string          compiledFunctionName;
    std::string          wrapperFunctionName;
    std::string          defineFunctionName;
    std::string          classBytesName;
    std::string          classRefName;
    std::string          doneFlagName;
    std::vector<uint8_t> classBytes;
};

static constexpr const char* kRegisterBridgeClassName     = "mj2x/cloader/Loader";
static constexpr const char* kRegisterBridgeMethodName    = "registerNativesForClass";
static constexpr const char* kRegisterBridgeMethodDesc    = "(ILjava/lang/Class;)V";
static constexpr const char* kRegisterBridgeCFunctionName = "_mj2x_register_for_class";
static constexpr const char* kHiddenClinitMethodName      = "__mj2x_hiddenClinit";
static constexpr const char* kHiddenClinitMethodDesc      = "(Ljava/lang/Class;)V";

static std::string          hiddenClinitClassNameFor(const std::string& ownerClassName);
static std::vector<uint8_t> makeHiddenClinitClassBytes(const std::string& hiddenClassName,
                                                       const std::string& hiddenMethodName);
static std::vector<uint8_t> makeHiddenInterfaceDefaultClassBytes(
    const std::string& hiddenClassName, const std::string& hiddenMethodName,
    const std::string& hiddenMethodDesc);
static void clearMethodDebugAndHandlers(MethodNode* method);

static std::string exceptionDispatchKey(const std::vector<int>& handlerIndices)
{
    std::ostringstream key;
    bool               first = true;
    for (int index : handlerIndices) {
        if (!first) key << ",";
        first = false;
        key << index;
    }
    return key.str();
}

static bool isProtectedByExceptionHandler(const MIRNode* node)
{
    switch (node->kind) {
    case MIRKind::LABEL:
    case MIRKind::FRAME:
    case MIRKind::LINE:
    case MIRKind::NOP:
    case MIRKind::BRANCH:
    case MIRKind::BRANCH_ZERO:
    case MIRKind::UNCOND_BRANCH:
    case MIRKind::SWITCH:
    case MIRKind::RETURN:
    case MIRKind::RETURN_VALUE: return false;
    default: return true;
    }
}

static void removeActiveHandler(std::vector<int>& activeHandlers, int handlerIndex)
{
    for (auto it = activeHandlers.begin(); it != activeHandlers.end(); ++it) {
        if (*it == handlerIndex) {
            activeHandlers.erase(it);
            return;
        }
    }
}

// ============================================================
// Impl
// ============================================================

struct StackCBackend::Impl
{
    std::ostringstream                                                header;
    std::ostringstream                                                source;
    std::unordered_set<std::string>                                   nativeFunctions;
    std::unordered_map<std::string, std::vector<CNativeRegistration>> registrationsByClass;
    std::vector<std::string>                                          registrationClassOrder;
    std::unordered_map<std::string, int>                              registrationClassIds;
    std::unordered_set<std::string>                                   protectedClinitClasses;
    std::unordered_map<std::string, CHiddenClinitProxy>               hiddenClinitByClass;
    std::vector<std::string>                                          hiddenClinitClassOrder;
    std::unordered_map<std::string, std::vector<CHiddenInterfaceDefaultProxy>>
                                            interfaceDefaultProxiesByClass;
    std::vector<std::string>                interfaceDefaultProxyClassOrder;
    std::unordered_map<std::string, size_t> stringPoolOffsets;
    std::vector<unsigned char>              stringPoolBytes;
    std::deque<std::string>                 stableUtf8Storage;
    std::string                             jarNativeLoaderResourcePath;
    std::string                             jarNativeLoaderTempSuffix;

    // Cache current method info for codegen
    std::string currentClassName;
    std::string currentMethodName;
    std::string currentMethodDesc;
    std::string currentExceptionDispatchTarget;

    void emitStackInit(int maxStack);
    void emitLocalsInit(const std::string& desc, int access);

    // MIR node → C code
    void   emitNode(const MIRNode* node, std::ostringstream& out);
    void   emitNodeWithExceptionDispatch(const MIRNode* node, std::ostringstream& out,
                                         const std::string& dispatchTarget);
    void   emitConstant(const MIRConstant* node, std::ostringstream& out);
    void   emitLocalLoad(const MIRLocalLoad* node, std::ostringstream& out);
    void   emitLocalStore(const MIRLocalStore* node, std::ostringstream& out);
    void   emitIinc(const MIRIinc* node, std::ostringstream& out);
    void   emitBinaryOp(const MIRBinaryOp* node, std::ostringstream& out);
    void   emitUnaryOp(const MIRUnaryOp* node, std::ostringstream& out);
    void   emitCmp(const MIRCmp* node, std::ostringstream& out);
    void   emitCast(const MIRCast* node, std::ostringstream& out);
    void   emitBranch(const MIRBranch* node, std::ostringstream& out);
    void   emitBranchZero(const MIRBranchZero* node, std::ostringstream& out);
    void   emitUncondBranch(const MIRUncondBranch* node, std::ostringstream& out);
    void   emitSwitch(const MIRSwitch* node, std::ostringstream& out);
    void   emitReturn(const MIRReturn* node, std::ostringstream& out);
    void   emitReturnValue(const MIRReturnValue* node, std::ostringstream& out);
    void   emitNewObject(const MIRNewObject* node, std::ostringstream& out);
    void   emitNewArray(const MIRNewArray* node, std::ostringstream& out);
    void   emitMultiNewArray(const MIRMultiNewArray* node, std::ostringstream& out);
    void   emitFieldGet(const MIRFieldGet* node, std::ostringstream& out);
    void   emitFieldPut(const MIRFieldPut* node, std::ostringstream& out);
    void   emitStaticGet(const MIRStaticGet* node, std::ostringstream& out);
    void   emitStaticPut(const MIRStaticPut* node, std::ostringstream& out);
    void   emitInvoke(const MIRInvoke* node, std::ostringstream& out);
    void   emitInvokeDynamic(const MIRInvokeDynamic* node, std::ostringstream& out);
    void   emitUnsupported(const char* what, std::ostringstream& out);
    void   emitCheckCast(const MIRCheckCast* node, std::ostringstream& out);
    void   emitInstanceOf(const MIRInstanceOf* node, std::ostringstream& out);
    void   emitMonitor(const MIRMonitor* node, std::ostringstream& out);
    void   emitThrow(const MIRThrow* node, std::ostringstream& out);
    void   emitArrayLength(const MIRArrayLength* node, std::ostringstream& out);
    void   emitArrayLoad(const MIRArrayLoad* node, std::ostringstream& out);
    void   emitArrayStore(const MIRArrayStore* node, std::ostringstream& out);
    void   emitIntegerDivZeroTrap(const char* rhsAccessor, std::ostringstream& out);
    void   emitNullPointerTrap(const std::string& refExpr, std::ostringstream& out);
    void   emitPendingExceptionCheck(std::ostringstream& out, int indent = 8);
    void   emitFieldIdCache(const std::string& cacheName, const std::string& localName,
                            const std::string& classExpr, const std::string& name,
                            const std::string& desc, bool isStatic, std::ostringstream& out);
    void   emitMethodIdCache(const std::string& cacheName, const std::string& localName,
                             const std::string& classExpr, const std::string& name,
                             const std::string& desc, bool isStatic, std::ostringstream& out);
    size_t stringPoolOffsetFor(const std::string& value);
    void   emitStringPool(std::ostringstream& out) const;
    void   emitRegistrationCode(std::ostringstream& out) const;
    ca::str::Utf8StringRef stableUtf8(const std::string& value);
    void                   protectClassInitializer(StackCBackend& backend, CompilerClass& clazz);
    void                   registerHiddenClinitProxy(const std::string& className,
                                                     const std::string& hiddenMethodName,
                                                     const std::string& compiledFunctionName);
    void                   registerInterfaceDefaultProxy(CompilerClass& clazz, MethodNode& method,
                                                         const std::string& compiledFunctionName);
    void        rewriteInterfaceDefaultMethodToProxy(CompilerClass& clazz, MethodNode& method,
                                                     const CHiddenInterfaceDefaultProxy& proxy);
    std::string findCompiledFunctionName(const std::string& className,
                                         const std::string& methodName,
                                         const std::string& methodDesc) const;
    bool        hasInterfaceDefaultProxies(const std::string& className) const;
    void        removeRegistration(const std::string& className, const std::string& methodName,
                                   const std::string& methodDesc);
    jbytecode::tree::MethodNode* findMethod(CompilerClass& clazz, const char* name,
                                            const char* desc = nullptr) const;
};

StackCBackend::StackCBackend()
    : impl_(std::make_unique<Impl>())
{}

StackCBackend::~StackCBackend() = default;

void StackCBackend::setJarNativeLoaderResourcePath(const std::string& resourcePath,
                                                   const std::string& tempSuffix)
{
    impl_->jarNativeLoaderResourcePath = resourcePath;
    impl_->jarNativeLoaderTempSuffix   = tempSuffix;
}

// ============================================================
// Main compile method
// ============================================================

BackendResult StackCBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                           const std::vector<Block*>& blocks)
{
    auto* mn                = method.methodNode;
    impl_->currentClassName = className;
    auto& cls               = impl_->currentClassName;

    impl_->currentMethodName = mn->name.to_std_string();
    impl_->currentMethodDesc = mn->desc.to_std_string();

    std::ostringstream funcOut;

    // JNI function signature
    bool isStatic = (mn->access & Opcodes::ACC_STATIC) != 0;
    auto retType =
        jvmDescToPrimitive(Type::getReturnType(mn->desc).getDescriptor().to_std_string());

    std::string methodName = mn->name.to_std_string();
    std::string methodDesc = mn->desc.to_std_string();
    auto        funcName   = nativeFunctionName(cls, methodName, methodDesc);
    if (!impl_->nativeFunctions.insert(funcName).second) {
        int         suffix = 1;
        std::string candidate;
        do {
            candidate = funcName + "_" + std::to_string(suffix++);
        } while (!impl_->nativeFunctions.insert(candidate).second);
        funcName = std::move(candidate);
    }
    auto [regsIt, insertedClass] =
        impl_->registrationsByClass.emplace(cls, std::vector<CNativeRegistration>{});
    if (insertedClass) {
        impl_->registrationClassIds.emplace(cls,
                                            static_cast<int>(impl_->registrationClassOrder.size()));
        impl_->registrationClassOrder.push_back(cls);
    }
    regsIt->second.push_back({methodName, methodDesc, funcName});

    funcOut << "static " << primitiveToJNIType(retType) << " JNICALL\n";
    funcOut << funcName << "(JNIEnv* env, " << (isStatic ? "jclass clazz" : "jobject this");

    // Add method parameters
    auto desc     = Type::getMethodType(mn->desc);
    auto argTypes = desc.getArgumentTypes();
    int  slot     = 0;
    int  paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut << ", " << primitiveToJNIType(pk) << " p" << paramIdx;
        slot += primitiveStackSize(pk);
        paramIdx++;
    }

    funcOut << ") {\n";

    // Stack and locals declaration
    int maxLocalsVal = mn->maxLocals > 0 ? mn->maxLocals : 1;
    funcOut << "    union jvalue stack[" << (mn->maxStack > 0 ? mn->maxStack : 1) << "];\n";
    funcOut << "    union jvalue locals[" << maxLocalsVal << "];\n";
    funcOut << "    int sp = -1;\n";
    funcOut << "    jobject _mj2x_refs[_MJ2X_LOCAL_REF_LIMIT];\n";
    funcOut << "    int _mj2x_ref_count = 0;\n";
    if (isStatic) {
        funcOut << "    jclass _mj2x_owner_class = clazz;\n\n";
    }
    else {
        funcOut << "    jclass _mj2x_owner_class = (jclass)"
                << trackLocalRefExpr("(*env)->GetObjectClass(env, this)") << ";\n\n";
    }

    // Param init
    slot = 0;
    if (!isStatic) {
        funcOut << "    locals[" << slot << "].l = this;\n";
        slot++;
    }
    paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        // 修复：原代码误写 stackAccessor(pk) + 1（指针运算，写出 ".i1" 之类的非法访问器）。
        funcOut << "    locals[" << slot << "]" << stackValueAccessor(pk) << " = ("
                << (narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk)) << ")p" << paramIdx
                << ";\n";
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut << "\n";

    std::vector<CExceptionHandlerInfo> exceptionHandlers;
    for (auto* tcb : mn->tryCatchBlocks) {
        CExceptionHandlerInfo h;
        h.start     = tcb->start;
        h.end       = tcb->end;
        h.handler   = tcb->handler;
        h.catchType = tcb->type.to_std_string();
        exceptionHandlers.push_back(std::move(h));
    }
    std::vector<int>                             activeExceptionHandlers;
    std::unordered_map<std::string, std::string> dispatchLabelByKey;
    std::vector<CExceptionDispatchInfo>          exceptionDispatches;
    auto dispatchTargetFor = [&](const std::vector<int>& handlers) -> std::string {
        if (handlers.empty()) return {};
        auto key   = exceptionDispatchKey(handlers);
        auto found = dispatchLabelByKey.find(key);
        if (found != dispatchLabelByKey.end()) return found->second;
        std::string label = "_mj2x_ex_dispatch_" + std::to_string(exceptionDispatches.size());
        dispatchLabelByKey.emplace(key, label);
        exceptionDispatches.push_back({label, handlers});
        return label;
    };

    // Emit blocks
    for (auto* block : blocks) {
        funcOut << "    /* B" << block->id << " */\n";

        for (auto& node : block->nodes) {
            if (node->kind == MIRKind::LABEL) {
                auto* label = static_cast<const MIRLabel*>(node.get())->labelNode;
                for (int i = 0; i < static_cast<int>(exceptionHandlers.size()); ++i) {
                    if (exceptionHandlers[static_cast<size_t>(i)].end == label)
                        removeActiveHandler(activeExceptionHandlers, i);
                }
                for (int i = 0; i < static_cast<int>(exceptionHandlers.size()); ++i) {
                    if (exceptionHandlers[static_cast<size_t>(i)].start == label)
                        activeExceptionHandlers.push_back(i);
                }
            }
            std::string dispatchTarget;
            if (!activeExceptionHandlers.empty() && isProtectedByExceptionHandler(node.get()))
                dispatchTarget = dispatchTargetFor(activeExceptionHandlers);
            impl_->emitNodeWithExceptionDispatch(node.get(), funcOut, dispatchTarget);
        }
        funcOut << "\n";
    }

    for (const auto& dispatch : exceptionDispatches) {
        funcOut << dispatch.label << ":\n";
        funcOut << "    {\n";
        funcOut << "        jthrowable _mj2x_ex = (jthrowable)"
                << trackLocalRefExpr("(*env)->ExceptionOccurred(env)") << ";\n";
        funcOut << "        (*env)->ExceptionClear(env);\n";
        funcOut << "        if (_mj2x_ex == NULL) {\n";
        emitExceptionReturn(impl_->currentMethodDesc, funcOut, 12);
        funcOut << "        }\n";
        for (int handlerIndex : dispatch.handlerIndices) {
            const auto& handler = exceptionHandlers[static_cast<size_t>(handlerIndex)];
            if (handler.catchType.empty()) {
                funcOut << "        sp = -1;\n";
                funcOut << "        stack[++sp].l = _mj2x_ex;\n";
                funcOut << "        goto " << labelIdent(handler.handler) << ";\n";
            }
            else {
                funcOut << "        if ((*env)->IsInstanceOf(env, _mj2x_ex, "
                        << classExprForDescriptor(handler.catchType) << ")) {\n";
                funcOut << "            sp = -1;\n";
                funcOut << "            stack[++sp].l = _mj2x_ex;\n";
                funcOut << "            goto " << labelIdent(handler.handler) << ";\n";
                funcOut << "        }\n";
            }
        }
        funcOut << "        (*env)->Throw(env, _mj2x_ex);\n";
        emitExceptionReturn(impl_->currentMethodDesc, funcOut, 8);
        funcOut << "    }\n\n";
    }

    funcOut << "}\n\n";
    impl_->source << funcOut.str();

    BackendResult result;
    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = funcOut.str();
    return result;
}

void StackCBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                    const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    impl_->protectClassInitializer(*this, clazz);

    auto* mn = method.methodNode;
    if (isInterfaceDefaultMethod(clazz, *mn)) {
        const auto className    = clazz.classNode->name.to_std_string();
        const auto functionName = impl_->findCompiledFunctionName(
            className, mn->name.to_std_string(), mn->desc.to_std_string());
        if (!functionName.empty()) {
            impl_->registerInterfaceDefaultProxy(clazz, *mn, functionName);
            const auto& proxies = impl_->interfaceDefaultProxiesByClass[className];
            impl_->rewriteInterfaceDefaultMethodToProxy(clazz, *mn, proxies.back());
        }
        return;
    }

    // Mark method as native and clear bytecode
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack   = 0;
    mn->maxLocals  = 0;
    clazz.modified = true;
}

ca::str::Utf8StringRef StackCBackend::Impl::stableUtf8(const std::string& value)
{
    stableUtf8Storage.push_back(value);
    return ca::str::Utf8StringRef::from_cstr(stableUtf8Storage.back().c_str());
}

jbytecode::tree::MethodNode* StackCBackend::Impl::findMethod(CompilerClass& clazz, const char* name,
                                                             const char* desc) const
{
    for (auto* method : clazz.classNode->methods) {
        if (method->name.to_std_string() != name) continue;
        if (desc != nullptr && method->desc.to_std_string() != desc) continue;
        return method;
    }
    return nullptr;
}

void StackCBackend::Impl::removeRegistration(const std::string& className,
                                             const std::string& methodName,
                                             const std::string& methodDesc)
{
    auto found = registrationsByClass.find(className);
    if (found == registrationsByClass.end()) return;
    auto& registrations = found->second;
    for (auto it = registrations.begin(); it != registrations.end(); ++it) {
        if (it->javaName == methodName && it->javaDesc == methodDesc) {
            registrations.erase(it);
            return;
        }
    }
}

void StackCBackend::Impl::registerHiddenClinitProxy(const std::string& className,
                                                    const std::string& hiddenMethodName,
                                                    const std::string& compiledFunctionName)
{
    CHiddenClinitProxy proxy;
    proxy.ownerClassName       = className;
    proxy.hiddenClassName      = hiddenClinitClassNameFor(className);
    proxy.hiddenMethodName     = kHiddenClinitMethodName;
    proxy.hiddenMethodDesc     = kHiddenClinitMethodDesc;
    proxy.compiledFunctionName = compiledFunctionName;

    const auto suffix         = hex64(fnv1a64(className));
    proxy.wrapperFunctionName = "_mj2x_hidden_clinit_wrapper_" + suffix;
    proxy.callFunctionName    = "_mj2x_call_hidden_clinit_" + suffix;
    proxy.classBytesName      = "_mj2x_hidden_clinit_class_" + suffix;
    proxy.classRefName        = "_mj2x_hidden_clinit_class_ref_" + suffix;
    proxy.methodIdName        = "_mj2x_hidden_clinit_mid_" + suffix;
    proxy.doneFlagName        = "_mj2x_hidden_clinit_done_" + suffix;
    proxy.classBytes = makeHiddenClinitClassBytes(proxy.hiddenClassName, proxy.hiddenMethodName);

    if (hiddenClinitByClass.emplace(className, std::move(proxy)).second)
        hiddenClinitClassOrder.push_back(className);
    removeRegistration(className, hiddenMethodName, "()V");
}

std::string StackCBackend::Impl::findCompiledFunctionName(const std::string& className,
                                                          const std::string& methodName,
                                                          const std::string& methodDesc) const
{
    auto regsIt = registrationsByClass.find(className);
    if (regsIt == registrationsByClass.end()) return {};
    for (auto it = regsIt->second.rbegin(); it != regsIt->second.rend(); ++it) {
        if (it->javaName == methodName && it->javaDesc == methodDesc) return it->cFunctionName;
    }
    return {};
}

bool StackCBackend::Impl::hasInterfaceDefaultProxies(const std::string& className) const
{
    auto found = interfaceDefaultProxiesByClass.find(className);
    return found != interfaceDefaultProxiesByClass.end() && !found->second.empty();
}

void StackCBackend::Impl::registerInterfaceDefaultProxy(CompilerClass& clazz, MethodNode& method,
                                                        const std::string& compiledFunctionName)
{
    const auto className  = clazz.classNode->name.to_std_string();
    const auto methodName = method.name.to_std_string();
    const auto methodDesc = method.desc.to_std_string();
    const auto suffix     = hex64(fnv1a64(className + "." + methodName + methodDesc));

    CHiddenInterfaceDefaultProxy proxy;
    proxy.ownerClassName       = className;
    proxy.javaName             = methodName;
    proxy.javaDesc             = methodDesc;
    proxy.hiddenClassName      = hiddenClinitClassNameFor(className + "$default$" + suffix);
    proxy.hiddenMethodName     = "__mj2x_default_" + suffix;
    proxy.hiddenMethodDesc     = prependReceiverToMethodDesc(className, methodDesc);
    proxy.compiledFunctionName = compiledFunctionName;
    proxy.wrapperFunctionName  = "_mj2x_interface_default_wrapper_" + suffix;
    proxy.defineFunctionName   = "_mj2x_define_interface_default_" + suffix;
    proxy.classBytesName       = "_mj2x_interface_default_class_" + suffix;
    proxy.classRefName         = "_mj2x_interface_default_class_ref_" + suffix;
    proxy.doneFlagName         = "_mj2x_interface_default_done_" + suffix;
    proxy.classBytes           = makeHiddenInterfaceDefaultClassBytes(
        proxy.hiddenClassName, proxy.hiddenMethodName, proxy.hiddenMethodDesc);

    auto [proxiesIt, inserted] = interfaceDefaultProxiesByClass.emplace(
        className, std::vector<CHiddenInterfaceDefaultProxy>{});
    if (inserted) interfaceDefaultProxyClassOrder.push_back(className);
    proxiesIt->second.push_back(std::move(proxy));
    removeRegistration(className, methodName, methodDesc);
}

void StackCBackend::Impl::rewriteInterfaceDefaultMethodToProxy(
    CompilerClass& clazz, MethodNode& method, const CHiddenInterfaceDefaultProxy& proxy)
{
    method.access &= ~Opcodes::ACC_NATIVE;
    method.instructions.clear();
    clearMethodDebugAndHandlers(&method);

    method.instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    int  slot     = 1;
    int  maxStack = 1;
    auto argTypes = Type::getArgumentTypes(method.desc);
    for (const auto& argType : argTypes) {
        method.instructions.add(new VarInsnNode(loadOpcodeForSort(argType.getSort()), slot));
        auto      pk    = jvmSortToPrimitive(argType.getSort());
        const int width = primitiveStackSize(pk);
        slot += width;
        maxStack += width;
    }
    method.instructions.add(new MethodInsnNode(Opcodes::INVOKESTATIC,
                                               stableUtf8(proxy.hiddenClassName),
                                               stableUtf8(proxy.hiddenMethodName),
                                               stableUtf8(proxy.hiddenMethodDesc),
                                               false));
    method.instructions.add(new InsnNode(returnOpcodeForKind(methodReturnKind(proxy.javaDesc))));
    method.maxStack  = maxStack;
    method.maxLocals = slot;
    clazz.modified   = true;
}

static void clearMethodDebugAndHandlers(MethodNode* method)
{
    for (auto* tc : method->tryCatchBlocks) delete tc;
    method->tryCatchBlocks.clear();
    for (auto* lv : method->localVariables) delete lv;
    method->localVariables.clear();
}

static std::string hiddenClinitClassNameFor(const std::string& ownerClassName)
{
    const auto slash      = ownerClassName.find_last_of('/');
    const auto simpleName = "__mj2xHiddenClinit" + hex64(fnv1a64(ownerClassName));
    if (slash == std::string::npos) return simpleName;
    return ownerClassName.substr(0, slash + 1) + simpleName;
}

static std::vector<uint8_t> makeHiddenClinitClassBytes(const std::string& hiddenClassName,
                                                       const std::string& hiddenMethodName)
{
    ca::str::Utf8StringArena arena;
    ClassNode                cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn.name      = arena.intern(hiddenClassName.c_str());
    cn.superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto*                               proxy = new MethodNode(
        Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE | Opcodes::ACC_SYNTHETIC,
        arena.intern(hiddenMethodName.c_str()),
        arena.intern(kHiddenClinitMethodDesc),
        arena.intern(""),
        noExceptions);
    proxy->maxStack  = 0;
    proxy->maxLocals = 1;
    cn.methods.push_back(proxy);

    ClassWriter writer(arena, 0);
    cn.accept(&writer);
    return writer.toByteArray();
}

static std::vector<uint8_t> makeHiddenInterfaceDefaultClassBytes(
    const std::string& hiddenClassName, const std::string& hiddenMethodName,
    const std::string& hiddenMethodDesc)
{
    ca::str::Utf8StringArena arena;
    ClassNode                cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn.name      = arena.intern(hiddenClassName.c_str());
    cn.superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto*                               proxy = new MethodNode(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE | Opcodes::ACC_SYNTHETIC,
        arena.intern(hiddenMethodName.c_str()),
        arena.intern(hiddenMethodDesc.c_str()),
        arena.intern(""),
        noExceptions);
    proxy->maxStack  = 0;
    proxy->maxLocals = 0;
    cn.methods.push_back(proxy);

    ClassWriter writer(arena, 0);
    cn.accept(&writer);
    return writer.toByteArray();
}

static void emitClassRegistrationCall(MethodNode* clinit, ca::str::Utf8StringRef className,
                                      const std::string& nativeLoaderResourcePath,
                                      const std::string& nativeLoaderTempSuffix, int classId)
{
    // NativeLoader.load 必须在 registerNativesForClass 之前执行（先加载 native 库
    // 才能绑定 native 方法）。clinit 此时已被 protectClassInitializer 清空，故用
    // add 顺序累积：load 指令在前，registerNatives 在后。
    if (!nativeLoaderResourcePath.empty()) {
        clinit->instructions.add(new LdcInsnNode(nativeLoaderResourcePath));
        clinit->instructions.add(new LdcInsnNode(nativeLoaderTempSuffix));
        clinit->instructions.add(
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               ca::str::ZUtf8StringRef::from_static(native_loader::kClassName),
                               ca::str::ZUtf8StringRef::from_static("load"),
                               ca::str::ZUtf8StringRef::from_static(native_loader::kLoadDesc),
                               false));
    }
    clinit->instructions.add(new LdcInsnNode(classId));
    clinit->instructions.add(new LdcInsnNode(Type::getObjectType(className)));
    clinit->instructions.add(
        new MethodInsnNode(Opcodes::INVOKESTATIC,
                           ca::str::ZUtf8StringRef::from_static(kRegisterBridgeClassName),
                           ca::str::ZUtf8StringRef::from_static(kRegisterBridgeMethodName),
                           ca::str::ZUtf8StringRef::from_static(kRegisterBridgeMethodDesc),
                           false));
}

void StackCBackend::Impl::protectClassInitializer(StackCBackend& backend, CompilerClass& clazz)
{
    const auto className = clazz.classNode->name.to_std_string();
    if (!protectedClinitClasses.insert(className).second) return;

    auto idIt = registrationClassIds.find(className);
    if (idIt == registrationClassIds.end()) return;

    auto*       clinit = findMethod(clazz, "<clinit>", "()V");
    std::string hiddenName;
    std::string hiddenFunctionName;

    if (clinit != nullptr && clinit->instructions.size() > 0) {
        hiddenName        = "__mj2x_hidden_clinit_" + hex64(fnv1a64(className));
        auto originalName = clinit->name;
        clinit->name      = stableUtf8(hiddenName);

        CompilerMethod hiddenMethod;
        hiddenMethod.methodNode       = clinit;
        hiddenMethod.markedForCompile = true;
        CodeConverter converter;
        auto          blocks = converter.convert(*clinit);
        (void)backend.compileMethod(className, hiddenMethod, blocks);
        auto regsIt = registrationsByClass.find(className);
        if (regsIt != registrationsByClass.end()) {
            for (auto it = regsIt->second.rbegin(); it != regsIt->second.rend(); ++it) {
                if (it->javaName == hiddenName && it->javaDesc == "()V") {
                    hiddenFunctionName = it->cFunctionName;
                    break;
                }
            }
        }
        clinit->name = originalName;
        if (!hiddenFunctionName.empty())
            registerHiddenClinitProxy(className, hiddenName, hiddenFunctionName);
    }

    if (clinit == nullptr) {
        std::vector<ca::str::Utf8StringRef> noExceptions;
        clinit = new MethodNode(Opcodes::ACC_STATIC,
                                ca::str::ZUtf8StringRef::from_static("<clinit>"),
                                ca::str::ZUtf8StringRef::from_static("()V"),
                                ca::str::ZUtf8StringRef::from_static(""),
                                noExceptions);
        clazz.classNode->methods.push_back(clinit);
    }

    clinit->access &= ~Opcodes::ACC_NATIVE;
    clinit->instructions.clear();
    clearMethodDebugAndHandlers(clinit);
    emitClassRegistrationCall(clinit,
                              clazz.classNode->name,
                              jarNativeLoaderResourcePath,
                              jarNativeLoaderTempSuffix,
                              idIt->second);
    clinit->instructions.add(new InsnNode(Opcodes::RETURN));
    clinit->maxStack  = 2;
    clinit->maxLocals = 0;
    clazz.modified    = true;
}

void StackCBackend::finalize(const CompilerIndex& index)
{
    // Build final source: header + all compiled function code
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdint.h");   // uint32_t/uint64_t（整数环绕运算）
    includes.addSystem("math.h");     // NAN/INFINITY（浮点常量）

    std::ostringstream full;
    full << "/* Generated by mj2x StackCBackend */\n";
    full << includes.render();
    full << "\n";
    full << backend::stackCRuntimePrelude();
    impl_->emitStringPool(full);
    full << impl_->source.str();
    impl_->emitRegistrationCode(full);

    headerCode_ = "/* Generated by mj2x StackCBackend */\n" + includes.render() + "\n";
    sourceCode_ = full.str();

    // Write to file if output path is set
    if (!cOutputPath_.empty()) {
        std::ofstream ofs(cOutputPath_, std::ios::binary);
        if (ofs) {
            ofs << sourceCode_;
            printf("[StackCBackend] Written: %s (%zu bytes)\n",
                   cOutputPath_.c_str(),
                   sourceCode_.size());
        }
        else {
            printf("[StackCBackend] ERROR: cannot write %s\n", cOutputPath_.c_str());
        }
    }
}

size_t StackCBackend::Impl::stringPoolOffsetFor(const std::string& value)
{
    auto found = stringPoolOffsets.find(value);
    if (found != stringPoolOffsets.end()) return found->second;

    const size_t offset = stringPoolBytes.size();
    stringPoolOffsets.emplace(value, offset);
    stringPoolBytes.insert(stringPoolBytes.end(), value.begin(), value.end());
    stringPoolBytes.push_back(0);
    return offset;
}

void StackCBackend::Impl::emitStringPool(std::ostringstream& out) const
{
    if (stringPoolBytes.empty()) return;

    out << "static const unsigned char _mj2x_string_pool[] = {";
    for (size_t i = 0; i < stringPoolBytes.size(); ++i) {
        if (i % 16 == 0) out << "\n    ";
        out << static_cast<unsigned int>(stringPoolBytes[i]);
        if (i + 1 < stringPoolBytes.size()) out << ", ";
    }
    out << "\n};\n\n";
}

void StackCBackend::Impl::emitRegistrationCode(std::ostringstream& out) const
{
    CSourceStream src;
    src.emitLine("/* Native registration */");
    src.emitLine("typedef jint (*_mj2x_reg_method)(JNIEnv* env, jclass clazz);");
    src.emitLine(
        "static _mj2x_reg_method _mj2x_reg_methods[" +
        std::to_string(registrationClassOrder.empty() ? 1 : registrationClassOrder.size()) +
        "] = {0};");
    src.emitLine("");
    for (const auto& className : hiddenClinitClassOrder) {
        auto hiddenIt = hiddenClinitByClass.find(className);
        if (hiddenIt == hiddenClinitByClass.end()) continue;
        const auto& proxy = hiddenIt->second;

        src.emitLine("static const unsigned char " + proxy.classBytesName + "[] = {");
        src.indent();
        std::string line;
        for (size_t i = 0; i < proxy.classBytes.size(); ++i) {
            if (line.empty()) {
                line = std::to_string(static_cast<unsigned int>(proxy.classBytes[i]));
            }
            else {
                line += ", " + std::to_string(static_cast<unsigned int>(proxy.classBytes[i]));
            }
            if (line.size() > 96) {
                src.emitLine(line + ",");
                line.clear();
            }
        }
        if (!line.empty()) src.emitLine(line);
        src.dedent();
        src.emitLine("};");
        src.emitLine("static jweak " + proxy.classRefName + " = NULL;");
        src.emitLine("static jmethodID " + proxy.methodIdName + " = NULL;");
        src.emitLine("static jboolean " + proxy.doneFlagName + " = JNI_FALSE;");
        src.emitLine("static void JNICALL " + proxy.wrapperFunctionName +
                     "(JNIEnv* env, jclass hidden_class, jclass owner_class) {");
        src.indent();
        src.emitLine("(void)hidden_class;");
        src.emitLine(proxy.compiledFunctionName + "(env, owner_class);");
        src.dedent();
        src.emitLine("}");
        src.emitLine("static jint " + proxy.callFunctionName +
                     "(JNIEnv* env, jclass owner_class) {");
        src.indent();
        src.emitLine("if (owner_class == NULL) return JNI_ERR;");
        src.emitLine("if ((*env)->MonitorEnter(env, owner_class) != 0) return JNI_ERR;");
        src.emitLine("jint result = JNI_OK;");
        src.emitLine("jclass cached_class = NULL;");
        src.emitLine("if (" + proxy.doneFlagName + ") {");
        src.indent();
        src.emitLine("goto " + proxy.callFunctionName + "_exit;");
        src.dedent();
        src.emitLine("}");
        src.emitLine("if (" + proxy.classRefName + " != NULL) {");
        src.indent();
        src.emitLine("cached_class = (jclass)(*env)->NewLocalRef(env, " + proxy.classRefName +
                     ");");
        src.emitLine("if (cached_class == NULL) {");
        src.indent();
        src.emitLine("(*env)->DeleteWeakGlobalRef(env, " + proxy.classRefName + ");");
        src.emitLine(proxy.classRefName + " = NULL;");
        src.emitLine(proxy.methodIdName + " = NULL;");
        src.dedent();
        src.emitLine("}");
        src.dedent();
        src.emitLine("}");
        src.emitLine("if (cached_class == NULL) {");
        src.indent();
        src.emitLine("jclass class_class = (*env)->FindClass(env, \"java/lang/Class\");");
        src.emitLine(
            "if ((*env)->ExceptionCheck(env) || class_class == NULL) { result = JNI_ERR; goto " +
            proxy.callFunctionName + "_exit; }");
        src.emitLine("jmethodID get_loader = (*env)->GetMethodID(env, class_class, "
                     "\"getClassLoader\", \"()Ljava/lang/ClassLoader;\");");
        src.emitLine(
            "if ((*env)->ExceptionCheck(env) || get_loader == NULL) { result = JNI_ERR; goto " +
            proxy.callFunctionName + "_exit; }");
        src.emitLine("jobject loader = (*env)->CallObjectMethod(env, owner_class, get_loader);");
        src.emitLine("if ((*env)->ExceptionCheck(env)) { result = JNI_ERR; goto " +
                     proxy.callFunctionName + "_exit; }");
        src.emitLine("jclass hidden_class = (*env)->DefineClass(env, " +
                     cStringLiteral(proxy.hiddenClassName) + ", loader, (const jbyte*)" +
                     proxy.classBytesName + ", (jsize)sizeof(" + proxy.classBytesName + "));");
        src.emitLine("if (loader != NULL) (*env)->DeleteLocalRef(env, loader);");
        src.emitLine("(*env)->DeleteLocalRef(env, class_class);");
        src.emitLine(
            "if ((*env)->ExceptionCheck(env) || hidden_class == NULL) { result = JNI_ERR; goto " +
            proxy.callFunctionName + "_exit; }");
        src.emitLine("cached_class = hidden_class;");
        src.emitLine("static JNINativeMethod hidden_methods[] = {");
        src.indent();
        src.emitLine("{(char*)" + cStringLiteral(proxy.hiddenMethodName) + ", (char*)" +
                     cStringLiteral(proxy.hiddenMethodDesc) + ", (void*)" +
                     proxy.wrapperFunctionName + "},");
        src.dedent();
        src.emitLine("};");
        src.emitLine(
            "if ((*env)->RegisterNatives(env, hidden_class, hidden_methods, "
            "sizeof(hidden_methods) / sizeof(hidden_methods[0])) != 0) { result = JNI_ERR; goto " +
            proxy.callFunctionName + "_exit; }");
        src.emitLine(proxy.methodIdName + " = (*env)->GetStaticMethodID(env, hidden_class, " +
                     cStringLiteral(proxy.hiddenMethodName) + ", " +
                     cStringLiteral(proxy.hiddenMethodDesc) + ");");
        src.emitLine("if ((*env)->ExceptionCheck(env) || " + proxy.methodIdName +
                     " == NULL) { result = JNI_ERR; goto " + proxy.callFunctionName + "_exit; }");
        src.emitLine(proxy.classRefName + " = (*env)->NewWeakGlobalRef(env, hidden_class);");
        src.emitLine("if (" + proxy.classRefName + " == NULL) { result = JNI_ERR; goto " +
                     proxy.callFunctionName + "_exit; }");
        src.dedent();
        src.emitLine("}");
        src.emitLine("(*env)->CallStaticVoidMethod(env, cached_class, " + proxy.methodIdName +
                     ", owner_class);");
        src.emitLine("if ((*env)->ExceptionCheck(env)) result = JNI_ERR;");
        src.emitLine("else " + proxy.doneFlagName + " = JNI_TRUE;");
        src.emitLine(proxy.callFunctionName + "_exit:");
        src.emitLine("if (cached_class != NULL) (*env)->DeleteLocalRef(env, cached_class);");
        src.emitLine("if ((*env)->MonitorExit(env, owner_class) != 0 && result == JNI_OK) result = "
                     "JNI_ERR;");
        src.emitLine("return result;");
        src.dedent();
        src.emitLine("}");
        src.emitLine("");
    }

    for (const auto& className : interfaceDefaultProxyClassOrder) {
        auto proxiesIt = interfaceDefaultProxiesByClass.find(className);
        if (proxiesIt == interfaceDefaultProxiesByClass.end()) continue;
        for (const auto& proxy : proxiesIt->second) {
            src.emitLine("static const unsigned char " + proxy.classBytesName + "[] = {");
            src.indent();
            std::string line;
            for (size_t i = 0; i < proxy.classBytes.size(); ++i) {
                if (line.empty()) {
                    line = std::to_string(static_cast<unsigned int>(proxy.classBytes[i]));
                }
                else {
                    line += ", " + std::to_string(static_cast<unsigned int>(proxy.classBytes[i]));
                }
                if (line.size() > 96) {
                    src.emitLine(line + ",");
                    line.clear();
                }
            }
            if (!line.empty()) src.emitLine(line);
            src.dedent();
            src.emitLine("};");
            src.emitLine("static jweak " + proxy.classRefName + " = NULL;");
            src.emitLine("static jboolean " + proxy.doneFlagName + " = JNI_FALSE;");

            const auto  retType   = methodReturnKind(proxy.javaDesc);
            std::string signature = "static " + std::string(primitiveToJNIType(retType)) +
                                    " JNICALL " + proxy.wrapperFunctionName +
                                    "(JNIEnv* env, jclass hidden_class, jobject receiver";
            auto argTypes =
                Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(proxy.javaDesc.c_str()));
            int paramIdx = 0;
            for (const auto& argType : argTypes) {
                auto pk = jvmSortToPrimitive(argType.getSort());
                signature +=
                    ", " + std::string(primitiveToJNIType(pk)) + " p" + std::to_string(paramIdx);
                ++paramIdx;
            }
            signature += ") {";
            src.emitLine(signature);
            src.indent();
            src.emitLine("(void)hidden_class;");
            std::string call = proxy.compiledFunctionName + "(env, receiver";
            for (int i = 0; i < paramIdx; ++i) call += ", p" + std::to_string(i);
            call += ")";
            if (retType == PrimitiveKind::VOID) {
                src.emitLine(call + ";");
                src.emitLine("return;");
            }
            else {
                src.emitLine("return " + call + ";");
            }
            src.dedent();
            src.emitLine("}");

            src.emitLine("static jint " + proxy.defineFunctionName +
                         "(JNIEnv* env, jclass owner_class) {");
            src.indent();
            src.emitLine("if (owner_class == NULL) return JNI_ERR;");
            src.emitLine("if ((*env)->MonitorEnter(env, owner_class) != 0) return JNI_ERR;");
            src.emitLine("jint result = JNI_OK;");
            src.emitLine("jclass cached_class = NULL;");
            src.emitLine("if (" + proxy.doneFlagName + ") goto " + proxy.defineFunctionName +
                         "_exit;");
            src.emitLine("if (" + proxy.classRefName + " != NULL) {");
            src.indent();
            src.emitLine("cached_class = (jclass)(*env)->NewLocalRef(env, " + proxy.classRefName +
                         ");");
            src.emitLine("if (cached_class == NULL) {");
            src.indent();
            src.emitLine("(*env)->DeleteWeakGlobalRef(env, " + proxy.classRefName + ");");
            src.emitLine(proxy.classRefName + " = NULL;");
            src.dedent();
            src.emitLine("}");
            src.dedent();
            src.emitLine("}");
            src.emitLine("if (cached_class == NULL) {");
            src.indent();
            src.emitLine("jclass class_class = (*env)->FindClass(env, \"java/lang/Class\");");
            src.emitLine("if ((*env)->ExceptionCheck(env) || class_class == NULL) { result = "
                         "JNI_ERR; goto " +
                         proxy.defineFunctionName + "_exit; }");
            src.emitLine("jmethodID get_loader = (*env)->GetMethodID(env, class_class, "
                         "\"getClassLoader\", \"()Ljava/lang/ClassLoader;\");");
            src.emitLine(
                "if ((*env)->ExceptionCheck(env) || get_loader == NULL) { result = JNI_ERR; goto " +
                proxy.defineFunctionName + "_exit; }");
            src.emitLine(
                "jobject loader = (*env)->CallObjectMethod(env, owner_class, get_loader);");
            src.emitLine("if ((*env)->ExceptionCheck(env)) { result = JNI_ERR; goto " +
                         proxy.defineFunctionName + "_exit; }");
            src.emitLine("jclass hidden_class = (*env)->DefineClass(env, " +
                         cStringLiteral(proxy.hiddenClassName) + ", loader, (const jbyte*)" +
                         proxy.classBytesName + ", (jsize)sizeof(" + proxy.classBytesName + "));");
            src.emitLine("if (loader != NULL) (*env)->DeleteLocalRef(env, loader);");
            src.emitLine("(*env)->DeleteLocalRef(env, class_class);");
            src.emitLine("if ((*env)->ExceptionCheck(env) || hidden_class == NULL) { result = "
                         "JNI_ERR; goto " +
                         proxy.defineFunctionName + "_exit; }");
            src.emitLine("cached_class = hidden_class;");
            src.emitLine("static JNINativeMethod hidden_methods[] = {");
            src.indent();
            src.emitLine("{(char*)" + cStringLiteral(proxy.hiddenMethodName) + ", (char*)" +
                         cStringLiteral(proxy.hiddenMethodDesc) + ", (void*)" +
                         proxy.wrapperFunctionName + "},");
            src.dedent();
            src.emitLine("};");
            src.emitLine("if ((*env)->RegisterNatives(env, hidden_class, hidden_methods, "
                         "sizeof(hidden_methods) / sizeof(hidden_methods[0])) != 0) { result = "
                         "JNI_ERR; goto " +
                         proxy.defineFunctionName + "_exit; }");
            src.emitLine(proxy.classRefName + " = (*env)->NewWeakGlobalRef(env, hidden_class);");
            src.emitLine("if (" + proxy.classRefName + " == NULL) { result = JNI_ERR; goto " +
                         proxy.defineFunctionName + "_exit; }");
            src.dedent();
            src.emitLine("}");
            src.emitLine(proxy.doneFlagName + " = JNI_TRUE;");
            src.emitLine(proxy.defineFunctionName + "_exit:");
            src.emitLine("if (cached_class != NULL) (*env)->DeleteLocalRef(env, cached_class);");
            src.emitLine("if ((*env)->MonitorExit(env, owner_class) != 0 && result == JNI_OK) "
                         "result = JNI_ERR;");
            src.emitLine("return result;");
            src.dedent();
            src.emitLine("}");
            src.emitLine("");
        }
    }

    for (const auto& className : registrationClassOrder) {
        auto       found = registrationsByClass.find(className);
        const bool hasRegularRegistrations =
            found != registrationsByClass.end() && !found->second.empty();
        if (!hasRegularRegistrations && !hasInterfaceDefaultProxies(className)) continue;

        const auto registerName = registerFunctionName(className);
        src.emitLine("static jint " + registerName + "(JNIEnv* env, jclass clazz) {");
        src.indent();
        src.emitLine("if (clazz == NULL) return JNI_ERR;");
        src.emitLine("if ((*env)->MonitorEnter(env, clazz) != 0) return JNI_ERR;");
        src.emitLine("jint result = JNI_OK;");
        if (hasRegularRegistrations) {
            src.emitLine("static JNINativeMethod methods[] = {");
            src.indent();
            for (const auto& reg : found->second) {
                src.emitLine("{(char*)" + cStringLiteral(reg.javaName) + ", (char*)" +
                             cStringLiteral(reg.javaDesc) + ", (void*)" + reg.cFunctionName + "},");
            }
            src.dedent();
            src.emitLine("};");
            src.emitLine("if ((*env)->RegisterNatives(env, clazz, methods, "
                         "sizeof(methods) / sizeof(methods[0])) != 0) {");
            src.indent();
            src.emitLine("result = JNI_ERR;");
            src.emitLine("goto " + registerName + "_exit;");
            src.dedent();
            src.emitLine("}");
        }
        auto proxyClassIt = interfaceDefaultProxiesByClass.find(className);
        if (proxyClassIt != interfaceDefaultProxiesByClass.end()) {
            for (const auto& proxy : proxyClassIt->second) {
                src.emitLine("if (" + proxy.defineFunctionName + "(env, clazz) != JNI_OK) {");
                src.indent();
                src.emitLine("result = JNI_ERR;");
                src.emitLine("goto " + registerName + "_exit;");
                src.dedent();
                src.emitLine("}");
            }
        }
        auto hiddenIt = hiddenClinitByClass.find(className);
        if (hiddenIt != hiddenClinitByClass.end()) {
            const auto& proxy = hiddenIt->second;
            src.emitLine("if (" + proxy.callFunctionName + "(env, clazz) != JNI_OK) {");
            src.indent();
            src.emitLine("result = JNI_ERR;");
            src.emitLine("goto " + registerName + "_exit;");
            src.dedent();
            src.emitLine("}");
        }
        src.emitLine(registerName + "_exit:");
        src.emitLine(
            "if ((*env)->MonitorExit(env, clazz) != 0 && result == JNI_OK) result = JNI_ERR;");
        src.emitLine("return result;");
        src.dedent();
        src.emitLine("}");
        src.emitLine("");
    }

    src.emitLine("static void JNICALL " + std::string(kRegisterBridgeCFunctionName) +
                 "(JNIEnv* env, jclass loader_class, jint class_id, jclass clazz) {");
    src.indent();
    src.emitLine("(void)loader_class;");
    src.emitLine(
        "if (class_id < 0 || class_id >= " + std::to_string(registrationClassOrder.size()) +
        " || _mj2x_reg_methods[class_id] == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class id "
                 "mismatch\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (clazz == NULL) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine(
        "if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native class is null\");");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("if (_mj2x_reg_methods[class_id](env, clazz) != JNI_OK) {");
    src.indent();
    src.emitLine("if (!(*env)->ExceptionCheck(env)) {");
    src.indent();
    src.emitLine("jclass ex_class = (*env)->FindClass(env, \"java/lang/UnsatisfiedLinkError\");");
    src.emitLine("if (ex_class != NULL) (*env)->ThrowNew(env, ex_class, \"mj2x native registration "
                 "failed\");");
    src.dedent();
    src.emitLine("}");
    src.emitLine("return;");
    src.dedent();
    src.emitLine("}");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    src.emitLine("JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {");
    src.indent();
    src.emitLine("JNIEnv* env = NULL;");
    src.emitLine("(void)reserved;");
    src.emitLine("if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;");
    for (const auto& className : registrationClassOrder) {
        auto       found = registrationsByClass.find(className);
        const bool hasRegularRegistrations =
            found != registrationsByClass.end() && !found->second.empty();
        if (!hasRegularRegistrations && !hasInterfaceDefaultProxies(className)) continue;
        auto idIt = registrationClassIds.find(className);
        if (idIt == registrationClassIds.end()) continue;
        src.emitLine("_mj2x_reg_methods[" + std::to_string(idIt->second) +
                     "] = " + registerFunctionName(className) + ";");
    }
    src.emitLine("jclass loader_class = (*env)->FindClass(env, " +
                 cStringLiteral(kRegisterBridgeClassName) + ");");
    src.emitLine("if ((*env)->ExceptionCheck(env) || loader_class == NULL) return JNI_ERR;");
    src.emitLine("static JNINativeMethod loader_methods[] = {");
    src.indent();
    src.emitLine("{(char*)" + cStringLiteral(kRegisterBridgeMethodName) + ", (char*)" +
                 cStringLiteral(kRegisterBridgeMethodDesc) + ", (void*)" +
                 kRegisterBridgeCFunctionName + "},");
    src.dedent();
    src.emitLine("};");
    src.emitLine("if ((*env)->RegisterNatives(env, loader_class, loader_methods, "
                 "sizeof(loader_methods) / sizeof(loader_methods[0])) != 0) return JNI_ERR;");
    src.emitLine("(*env)->DeleteLocalRef(env, loader_class);");
    src.emitLine("return JNI_VERSION_1_6;");
    src.dedent();
    src.emitLine("}");
    src.emitLine("");

    out << src.str();
}

// ============================================================
// MIR Node → C code
// ============================================================

void StackCBackend::Impl::emitNode(const MIRNode* node, std::ostringstream& out)
{
    switch (node->kind) {
    case MIRKind::LABEL:
        out << labelIdent(static_cast<const MIRLabel*>(node)->labelNode) << ":\n";
        out << "    ;\n";
        break;
    case MIRKind::CONSTANT: emitConstant(static_cast<const MIRConstant*>(node), out); break;
    case MIRKind::LOCAL_LOAD: emitLocalLoad(static_cast<const MIRLocalLoad*>(node), out); break;
    case MIRKind::LOCAL_STORE: emitLocalStore(static_cast<const MIRLocalStore*>(node), out); break;
    case MIRKind::IINC: emitIinc(static_cast<const MIRIinc*>(node), out); break;
    case MIRKind::STACK_DUP: out << "    stack[sp + 1] = stack[sp]; sp++;\n"; break;
    case MIRKind::STACK_DUP_X1:
        out << "    {\n";
        out << "        union jvalue v1 = stack[sp];\n";
        out << "        union jvalue v2 = stack[sp - 1];\n";
        out << "        stack[sp - 1] = v1;\n";
        out << "        stack[sp] = v2;\n";
        out << "        stack[sp + 1] = v1;\n";
        out << "        sp++;\n";
        out << "    }\n";
        break;
    case MIRKind::STACK_POP: out << "    sp--;\n"; break;
    case MIRKind::STACK_POP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        out << "    sp -= " << (stackOpTopIsCategory2(op) ? 1 : 2) << ";\n";
        break;
    }
    case MIRKind::STACK_SWAP:
    {
        out << "    {\n";
        out << "        union jvalue tmp = stack[sp];\n";
        out << "        stack[sp] = stack[sp - 1];\n";
        out << "        stack[sp - 1] = tmp;\n";
        out << "    }\n";
        break;
    }
    case MIRKind::STACK_DUP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out << "    stack[sp + 1] = stack[sp]; sp++;\n";
        }
        else {
            out << "    stack[sp + 1] = stack[sp - 1];\n";
            out << "    stack[sp + 2] = stack[sp];\n";
            out << "    sp += 2;\n";
        }
        break;
    }
    case MIRKind::STACK_DUP2_X1:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        stack[sp - 1] = v1;\n";
            out << "        stack[sp] = v2;\n";
            out << "        stack[sp + 1] = v1;\n";
            out << "        sp++;\n";
            out << "    }\n";
        }
        else {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        union jvalue v3 = stack[sp - 2];\n";
            out << "        stack[sp - 2] = v2;\n";
            out << "        stack[sp - 1] = v1;\n";
            out << "        stack[sp] = v3;\n";
            out << "        stack[sp + 1] = v2;\n";
            out << "        stack[sp + 2] = v1;\n";
            out << "        sp += 2;\n";
            out << "    }\n";
        }
        break;
    }
    case MIRKind::STACK_DUP2_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            if (stackOpTopIsCategory2(op, 1)) {
                out << "    {\n";
                out << "        union jvalue v1 = stack[sp];\n";
                out << "        union jvalue v2 = stack[sp - 1];\n";
                out << "        stack[sp - 1] = v1;\n";
                out << "        stack[sp] = v2;\n";
                out << "        stack[sp + 1] = v1;\n";
                out << "        sp++;\n";
                out << "    }\n";
            }
            else {
                out << "    {\n";
                out << "        union jvalue v1 = stack[sp];\n";
                out << "        union jvalue v2 = stack[sp - 1];\n";
                out << "        union jvalue v3 = stack[sp - 2];\n";
                out << "        stack[sp - 2] = v1;\n";
                out << "        stack[sp - 1] = v3;\n";
                out << "        stack[sp] = v2;\n";
                out << "        stack[sp + 1] = v1;\n";
                out << "        sp++;\n";
                out << "    }\n";
            }
        }
        else if (stackOpTopIsCategory2(op, 2)) {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        union jvalue v3 = stack[sp - 2];\n";
            out << "        stack[sp - 2] = v2;\n";
            out << "        stack[sp - 1] = v1;\n";
            out << "        stack[sp] = v3;\n";
            out << "        stack[sp + 1] = v2;\n";
            out << "        stack[sp + 2] = v1;\n";
            out << "        sp += 2;\n";
            out << "    }\n";
        }
        else {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        union jvalue v3 = stack[sp - 2];\n";
            out << "        union jvalue v4 = stack[sp - 3];\n";
            out << "        stack[sp - 3] = v2;\n";
            out << "        stack[sp - 2] = v1;\n";
            out << "        stack[sp - 1] = v4;\n";
            out << "        stack[sp] = v3;\n";
            out << "        stack[sp + 1] = v2;\n";
            out << "        stack[sp + 2] = v1;\n";
            out << "        sp += 2;\n";
            out << "    }\n";
        }
        break;
    }
    case MIRKind::STACK_DUP_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op, 1)) {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        stack[sp - 1] = v1;\n";
            out << "        stack[sp] = v2;\n";
            out << "        stack[sp + 1] = v1;\n";
            out << "        sp++;\n";
            out << "    }\n";
        }
        else {
            out << "    {\n";
            out << "        union jvalue v1 = stack[sp];\n";
            out << "        union jvalue v2 = stack[sp - 1];\n";
            out << "        union jvalue v3 = stack[sp - 2];\n";
            out << "        stack[sp - 2] = v1;\n";
            out << "        stack[sp - 1] = v3;\n";
            out << "        stack[sp] = v2;\n";
            out << "        stack[sp + 1] = v1;\n";
            out << "        sp++;\n";
            out << "    }\n";
        }
        break;
    }
    case MIRKind::BINARY_OP: emitBinaryOp(static_cast<const MIRBinaryOp*>(node), out); break;
    case MIRKind::UNARY_OP: emitUnaryOp(static_cast<const MIRUnaryOp*>(node), out); break;
    case MIRKind::CMP: emitCmp(static_cast<const MIRCmp*>(node), out); break;
    case MIRKind::CAST: emitCast(static_cast<const MIRCast*>(node), out); break;
    case MIRKind::BRANCH: emitBranch(static_cast<const MIRBranch*>(node), out); break;
    case MIRKind::BRANCH_ZERO: emitBranchZero(static_cast<const MIRBranchZero*>(node), out); break;
    case MIRKind::UNCOND_BRANCH:
        emitUncondBranch(static_cast<const MIRUncondBranch*>(node), out);
        break;
    case MIRKind::SWITCH: emitSwitch(static_cast<const MIRSwitch*>(node), out); break;
    case MIRKind::RETURN: out << renderStackCSnippet(StackCSnippet::ReturnVoid, {}); break;
    case MIRKind::RETURN_VALUE:
        emitReturnValue(static_cast<const MIRReturnValue*>(node), out);
        break;
    case MIRKind::NEW_OBJECT: emitNewObject(static_cast<const MIRNewObject*>(node), out); break;
    case MIRKind::NEW_ARRAY: emitNewArray(static_cast<const MIRNewArray*>(node), out); break;
    case MIRKind::MULTI_NEW_ARRAY:
        emitMultiNewArray(static_cast<const MIRMultiNewArray*>(node), out);
        break;
    case MIRKind::ARRAY_LENGTH:
        emitArrayLength(static_cast<const MIRArrayLength*>(node), out);
        break;
    case MIRKind::ARRAY_LOAD: emitArrayLoad(static_cast<const MIRArrayLoad*>(node), out); break;
    case MIRKind::ARRAY_STORE: emitArrayStore(static_cast<const MIRArrayStore*>(node), out); break;
    case MIRKind::FIELD_GET: emitFieldGet(static_cast<const MIRFieldGet*>(node), out); break;
    case MIRKind::FIELD_PUT: emitFieldPut(static_cast<const MIRFieldPut*>(node), out); break;
    case MIRKind::STATIC_GET: emitStaticGet(static_cast<const MIRStaticGet*>(node), out); break;
    case MIRKind::STATIC_PUT: emitStaticPut(static_cast<const MIRStaticPut*>(node), out); break;
    case MIRKind::INVOKE: emitInvoke(static_cast<const MIRInvoke*>(node), out); break;
    case MIRKind::INVOKE_DYNAMIC:
        emitInvokeDynamic(static_cast<const MIRInvokeDynamic*>(node), out);
        break;
    case MIRKind::CHECK_CAST: emitCheckCast(static_cast<const MIRCheckCast*>(node), out); break;
    case MIRKind::INSTANCE_OF: emitInstanceOf(static_cast<const MIRInstanceOf*>(node), out); break;
    case MIRKind::MONITOR_ENTER:
    case MIRKind::MONITOR_EXIT: emitMonitor(static_cast<const MIRMonitor*>(node), out); break;
    case MIRKind::THROW: emitThrow(static_cast<const MIRThrow*>(node), out); break;
    default: break;
    }
}

void StackCBackend::Impl::emitNodeWithExceptionDispatch(const MIRNode*      node,
                                                        std::ostringstream& out,
                                                        const std::string&  dispatchTarget)
{
    currentExceptionDispatchTarget = dispatchTarget;
    emitNode(node, out);
    currentExceptionDispatchTarget.clear();
    if (!dispatchTarget.empty() && isProtectedByExceptionHandler(node)) {
        out << "    if ((*env)->ExceptionCheck(env)) goto " << dispatchTarget << ";\n";
    }
}

void StackCBackend::Impl::emitConstant(const MIRConstant* node, std::ostringstream& out)
{
    const char* acc = stackAccessor(node->type);
    if (std::holds_alternative<int64_t>(node->value)) {
        auto v = std::get<int64_t>(node->value);
        if (node->type == PrimitiveKind::LONG) {
            // 用 unsigned 64-bit 字面量再转 jlong，避免 INT64_MIN 被解析成越界有符号字面量。
            auto u = static_cast<uint64_t>(v);
            out << renderStackCSnippet(
                StackCSnippet::PushStackValue,
                {{"acc", acc}, {"value", "(jlong)" + std::to_string(u) + "ULL"}});
        }
        else {
            // int：先转 uint32 再转 jint，保证 INT_MIN 等值无符号环绕语义、字面量不越界。
            auto u = static_cast<uint32_t>(static_cast<int32_t>(v));
            out << renderStackCSnippet(
                StackCSnippet::PushStackValue,
                {{"acc", acc}, {"value", "(jint)" + std::to_string(u) + "u"}});
        }
    }
    else if (std::holds_alternative<double>(node->value)) {
        auto d = std::get<double>(node->value);
        if (node->type == PrimitiveKind::FLOAT) {
            // float 字面量加 f 后缀；NaN/Inf 走宏保证可移植（MSVC/gcc 均支持）。
            std::ostringstream value;
            if (std::isnan(d))
                value << "NAN";
            else if (std::isinf(d))
                value << "(" << (d < 0 ? "-" : "") << "INFINITY)";
            else
                value << std::setprecision(9) << d << "f";
            out << renderStackCSnippet(StackCSnippet::PushStackValue,
                                       {{"acc", acc}, {"value", value.str()}});
        }
        else {
            std::ostringstream value;
            if (std::isnan(d))
                value << "(double)NAN";
            else if (std::isinf(d))
                value << "(" << (d < 0 ? "-" : "") << "(double)INFINITY)";
            else
                value << std::setprecision(17) << d;
            out << renderStackCSnippet(StackCSnippet::PushStackValue,
                                       {{"acc", acc}, {"value", value.str()}});
        }
    }
    else if (std::holds_alternative<std::string>(node->value)) {
        const auto offset = stringPoolOffsetFor(std::get<std::string>(node->value));
        out << "    stack[++sp].l = "
            << trackLocalRefExpr("(*env)->NewStringUTF(env, (const char*)(_mj2x_string_pool + " +
                                 std::to_string(offset) + "))")
            << ";\n";
    }
    else if (std::holds_alternative<Type>(node->value)) {
        const auto type = std::get<Type>(node->value);
        if (type.getSort() == Type::METHOD) {
            out << renderStackCSnippet(StackCSnippet::PushStackValue,
                                       {{"acc", acc}, {"value", "NULL"}});
            return;
        }
        if (type.getSort() == Type::OBJECT || type.getSort() == Type::ARRAY ||
            type.getSort() == Type::INTERNAL) {
            out << "    stack[++sp].l = "
                << classExprForDescriptor(type.getDescriptor().to_std_string()) << ";\n";
            return;
        }
        const auto kind = jvmSortToPrimitive(type.getSort());
        out << "    {\n";
        out << "        jclass wrapper = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"" + std::string(boxedClassName(kind)) +
                                 "\")")
            << ";\n";
        out << "        jfieldID type_fid = (*env)->GetStaticFieldID(env, wrapper, \"TYPE\", "
               "\"Ljava/lang/Class;\");\n";
        out << "        stack[++sp].l = "
            << trackLocalRefExpr("(*env)->GetStaticObjectField(env, wrapper, type_fid)") << ";\n";
        out << "    }\n";
    }
    else {
        out << renderStackCSnippet(StackCSnippet::PushStackValue,
                                   {{"acc", acc}, {"value", "NULL"}});
    }
}

void StackCBackend::Impl::emitLocalLoad(const MIRLocalLoad* node, std::ostringstream& out)
{
    out << renderStackCSnippet(
        StackCSnippet::LocalLoad,
        {{"acc", stackAccessor(node->type)}, {"slot", std::to_string(node->varIndex)}});
}

void StackCBackend::Impl::emitLocalStore(const MIRLocalStore* node, std::ostringstream& out)
{
    out << renderStackCSnippet(
        StackCSnippet::LocalStore,
        {{"acc", stackAccessor(node->type)}, {"slot", std::to_string(node->varIndex)}});
}

void StackCBackend::Impl::emitIinc(const MIRIinc* node, std::ostringstream& out)
{
    out << "    locals[" << node->varIndex << "].i += " << node->increment << ";\n";
}

void StackCBackend::Impl::emitIntegerDivZeroTrap(const char* rhsAccessor, std::ostringstream& out)
{
    out << "    if (stack[sp]" << rhsAccessor << " == 0) {\n";
    out << "        jclass ex_class = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/ArithmeticException\")") << ";\n";
    out << "        (*env)->ThrowNew(env, ex_class, \"/ by zero\");\n";
    if (!currentExceptionDispatchTarget.empty())
        out << "        goto " << currentExceptionDispatchTarget << ";\n";
    else
        emitExceptionReturn(currentMethodDesc, out, 8);
    out << "    }\n";
}

void StackCBackend::Impl::emitNullPointerTrap(const std::string& refExpr, std::ostringstream& out)
{
    std::ostringstream dispatch;
    if (!currentExceptionDispatchTarget.empty())
        dispatch << "        goto " << currentExceptionDispatchTarget << ";\n";
    else
        emitExceptionReturn(currentMethodDesc, dispatch, 8);
    out << renderStackCSnippet(
        StackCSnippet::NullPointerTrap,
        {{"ref", refExpr},
         {"ex_class",
          trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/NullPointerException\")")},
         {"dispatch", dispatch.str()}});
}

void StackCBackend::Impl::emitPendingExceptionCheck(std::ostringstream& out, int indent)
{
    const auto         pad = std::string(static_cast<size_t>(indent), ' ');
    std::ostringstream dispatch;
    if (!currentExceptionDispatchTarget.empty()) {
        dispatch << pad << "    goto " << currentExceptionDispatchTarget << ";\n";
    }
    else {
        emitExceptionReturn(currentMethodDesc, dispatch, indent + 4);
    }
    out << renderStackCSnippet(StackCSnippet::PendingExceptionCheck,
                               {{"pad", pad}, {"dispatch", dispatch.str()}});
}

void StackCBackend::Impl::emitFieldIdCache(const std::string& cacheName,
                                           const std::string& localName,
                                           const std::string& classExpr, const std::string& name,
                                           const std::string& desc, bool isStatic,
                                           std::ostringstream& out)
{
    CSourceStream src;
    src.indent();
    src.indent();
    const std::string staticPrefix = isStatic ? "Static" : "";
    out << "        if ((*env)->ExceptionCheck(env)) {\n";
    if (!currentExceptionDispatchTarget.empty()) {
        out << "            goto " << currentExceptionDispatchTarget << ";\n";
    }
    else {
        emitExceptionReturn(currentMethodDesc, out, 12);
    }
    out << "        }\n";
    out << "        if (" << classExpr << " == NULL) {\n";
    out << "            jclass ex_class = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/NoClassDefFoundError\")") << ";\n";
    out << "            (*env)->ThrowNew(env, ex_class, \"class lookup failed\");\n";
    if (!currentExceptionDispatchTarget.empty()) {
        out << "            goto " << currentExceptionDispatchTarget << ";\n";
    }
    else {
        emitExceptionReturn(currentMethodDesc, out, 12);
    }
    out << "        }\n";
    src.emitLine("static jfieldID " + cacheName + " = NULL;");
    src.emitLine("jfieldID " + localName + " = NULL;");
    src.emitLine("if ((*env)->MonitorEnter(env, _mj2x_owner_class) == 0) {");
    src.indent();
    src.emitLine("if (" + cacheName + " == NULL) " + cacheName + " = (*env)->Get" + staticPrefix +
                 "FieldID(env, " + classExpr + ", " + cStringLiteral(name) + ", " +
                 cStringLiteral(desc) + ");");
    src.emitLine(localName + " = " + cacheName + ";");
    src.emitLine("if ((*env)->MonitorExit(env, _mj2x_owner_class) != 0) " + localName + " = NULL;");
    src.dedent();
    src.emitLine("}");
    out << src.str();
    emitPendingExceptionCheck(out, 8);
}

void StackCBackend::Impl::emitMethodIdCache(const std::string& cacheName,
                                            const std::string& localName,
                                            const std::string& classExpr, const std::string& name,
                                            const std::string& desc, bool isStatic,
                                            std::ostringstream& out)
{
    CSourceStream src;
    src.indent();
    src.indent();
    const std::string staticPrefix = isStatic ? "Static" : "";
    out << "        if ((*env)->ExceptionCheck(env)) {\n";
    if (!currentExceptionDispatchTarget.empty()) {
        out << "            goto " << currentExceptionDispatchTarget << ";\n";
    }
    else {
        emitExceptionReturn(currentMethodDesc, out, 12);
    }
    out << "        }\n";
    out << "        if (" << classExpr << " == NULL) {\n";
    out << "            jclass ex_class = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/NoClassDefFoundError\")") << ";\n";
    out << "            (*env)->ThrowNew(env, ex_class, \"class lookup failed\");\n";
    if (!currentExceptionDispatchTarget.empty()) {
        out << "            goto " << currentExceptionDispatchTarget << ";\n";
    }
    else {
        emitExceptionReturn(currentMethodDesc, out, 12);
    }
    out << "        }\n";
    src.emitLine("static jmethodID " + cacheName + " = NULL;");
    src.emitLine("jmethodID " + localName + " = NULL;");
    src.emitLine("if ((*env)->MonitorEnter(env, _mj2x_owner_class) == 0) {");
    src.indent();
    src.emitLine("if (" + cacheName + " == NULL) " + cacheName + " = (*env)->Get" + staticPrefix +
                 "MethodID(env, " + classExpr + ", " + cStringLiteral(name) + ", " +
                 cStringLiteral(desc) + ");");
    src.emitLine(localName + " = " + cacheName + ";");
    src.emitLine("if ((*env)->MonitorExit(env, _mj2x_owner_class) != 0) " + localName + " = NULL;");
    src.dedent();
    src.emitLine("}");
    out << src.str();
    emitPendingExceptionCheck(out, 8);
}

void StackCBackend::Impl::emitBinaryOp(const MIRBinaryOp* node, std::ostringstream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* lhs = "stack[sp - 1]";
    const char* rhs = "stack[sp]";

    // JVM 整数算术是确定的有符号二补码环绕；C int 溢出是 UB。
    // 对 ADD/SUB/MUL/SHL/NEG/USHR 显式走无符号运算再转回，保证环绕语义。
    // DIV/REM：除零必须抛 ArithmeticException；MIN_VALUE / -1 和 % -1 也显式处理，
    // 避免 C 有符号除法 UB。
    if (node->type == PrimitiveKind::INT) {
        switch (node->op) {
        case BinaryOp::ADD:
        case BinaryOp::SUB:
        case BinaryOp::MUL:
            out << renderStackCSnippet(StackCSnippet::BinaryUnsignedWrap,
                                       {{"lhs", lhs},
                                        {"rhs", rhs},
                                        {"acc", acc},
                                        {"signed_type", "jint"},
                                        {"unsigned_type", "uint32_t"},
                                        {"op", binaryOpToC(node->op)}});
            return;
        case BinaryOp::SHL:
            out << "    " << lhs << acc << " = (jint)((uint32_t)" << lhs << acc << " << (" << rhs
                << acc << " & 31));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::SHR:
            out << "    " << lhs << acc << " = (jint)(" << lhs << acc << " >> (" << rhs << acc
                << " & 31));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::USHR:
            // 无符号右移：先把操作数转 uint32 再 >> 。
            out << "    " << lhs << acc << " = (jint)((uint32_t)" << lhs << acc << " >> (" << rhs
                << acc << " & 31));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::DIV:
            emitIntegerDivZeroTrap(acc, out);
            out << "    if (" << lhs << acc << " == (jint)0x80000000u && " << rhs << acc
                << " == -1) {\n";
            out << "        " << lhs << acc << " = (jint)0x80000000u;\n";
            out << "    } else {\n";
            out << "        " << lhs << acc << " = " << lhs << acc << " / " << rhs << acc << ";\n";
            out << "    }\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::REM:
            emitIntegerDivZeroTrap(acc, out);
            out << "    if (" << lhs << acc << " == (jint)0x80000000u && " << rhs << acc
                << " == -1) {\n";
            out << "        " << lhs << acc << " = 0;\n";
            out << "    } else {\n";
            out << "        " << lhs << acc << " = " << lhs << acc << " % " << rhs << acc << ";\n";
            out << "    }\n";
            out << "    sp--;\n";
            return;
        default: break;
        }
    }
    if (node->type == PrimitiveKind::LONG) {
        switch (node->op) {
        case BinaryOp::ADD:
        case BinaryOp::SUB:
        case BinaryOp::MUL:
            out << renderStackCSnippet(StackCSnippet::BinaryUnsignedWrap,
                                       {{"lhs", lhs},
                                        {"rhs", rhs},
                                        {"acc", acc},
                                        {"signed_type", "jlong"},
                                        {"unsigned_type", "uint64_t"},
                                        {"op", binaryOpToC(node->op)}});
            return;
        case BinaryOp::SHL:
            out << "    " << lhs << acc << " = (jlong)((uint64_t)" << lhs << acc << " << (" << rhs
                << ".i & 63));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::SHR:
            out << "    " << lhs << acc << " = (jlong)(" << lhs << acc << " >> (" << rhs
                << ".i & 63));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::USHR:
            out << "    " << lhs << acc << " = (jlong)((uint64_t)" << lhs << acc << " >> (" << rhs
                << ".i & 63));\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::DIV:
            emitIntegerDivZeroTrap(acc, out);
            out << "    if (" << lhs << acc << " == (jlong)0x8000000000000000ULL && " << rhs << acc
                << " == -1) {\n";
            out << "        " << lhs << acc << " = (jlong)0x8000000000000000ULL;\n";
            out << "    } else {\n";
            out << "        " << lhs << acc << " = " << lhs << acc << " / " << rhs << acc << ";\n";
            out << "    }\n";
            out << "    sp--;\n";
            return;
        case BinaryOp::REM:
            emitIntegerDivZeroTrap(acc, out);
            out << "    if (" << lhs << acc << " == (jlong)0x8000000000000000ULL && " << rhs << acc
                << " == -1) {\n";
            out << "        " << lhs << acc << " = 0;\n";
            out << "    } else {\n";
            out << "        " << lhs << acc << " = " << lhs << acc << " % " << rhs << acc << ";\n";
            out << "    }\n";
            out << "    sp--;\n";
            return;
        default: break;
        }
    }
    if (node->type == PrimitiveKind::FLOAT && node->op == BinaryOp::REM) {
        out << "    " << lhs << acc << " = fmodf(" << lhs << acc << ", " << rhs << acc << ");\n";
        out << "    sp--;\n";
        return;
    }
    if (node->type == PrimitiveKind::DOUBLE && node->op == BinaryOp::REM) {
        out << "    " << lhs << acc << " = fmod(" << lhs << acc << ", " << rhs << acc << ");\n";
        out << "    sp--;\n";
        return;
    }
    // float/double 或 DIV/REM/SHR/AND/OR/XOR：直接发原生运算符。
    out << "    " << lhs << acc << " = " << lhs << acc << " " << binaryOpToC(node->op) << " " << rhs
        << acc << ";\n";
    out << "    sp--;\n";
}

void StackCBackend::Impl::emitCmp(const MIRCmp* node, std::ostringstream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* lhs = "stack[sp - 1]";
    const char* rhs = "stack[sp]";
    if (node->type == PrimitiveKind::FLOAT || node->type == PrimitiveKind::DOUBLE) {
        out << "    if (isnan((double)" << lhs << acc << ") || isnan((double)" << rhs << acc
            << ")) {\n";
        out << "        " << lhs << ".i = " << (node->nanGreater ? "1" : "-1") << ";\n";
        out << "    } else {\n";
        out << "        " << lhs << ".i = (" << lhs << acc << " > " << rhs << acc << ") - (" << lhs
            << acc << " < " << rhs << acc << ");\n";
        out << "    }\n";
    }
    else {
        out << "    " << lhs << ".i = (" << lhs << acc << " > " << rhs << acc << ") - (" << lhs
            << acc << " < " << rhs << acc << ");\n";
    }
    out << "    sp--;\n";
}

void StackCBackend::Impl::emitCast(const MIRCast* node, std::ostringstream& out)
{
    const char* fromAcc = stackAccessor(node->from);
    const char* toAcc   = castResultAccessor(node->to);
    if (node->from == PrimitiveKind::FLOAT || node->from == PrimitiveKind::DOUBLE) {
        if (node->to == PrimitiveKind::INT) {
            out << "    {\n";
            out << "        jdouble v = (jdouble)stack[sp]" << fromAcc << ";\n";
            out << "        if (isnan(v)) stack[sp].i = 0;\n";
            out << "        else if (v > 2147483647.0) stack[sp].i = 2147483647;\n";
            out << "        else if (v < -2147483648.0) stack[sp].i = (jint)2147483648u;\n";
            out << "        else stack[sp].i = (jint)v;\n";
            out << "    }\n";
            return;
        }
        if (node->to == PrimitiveKind::LONG) {
            out << "    {\n";
            out << "        jdouble v = (jdouble)stack[sp]" << fromAcc << ";\n";
            out << "        if (isnan(v)) stack[sp].j = 0;\n";
            out << "        else if (v >= 9223372036854775808.0) stack[sp].j = "
                   "(jlong)0x7fffffffffffffffLL;\n";
            out << "        else if (v <= -9223372036854775808.0) stack[sp].j = "
                   "(jlong)0x8000000000000000ULL;\n";
            out << "        else stack[sp].j = (jlong)v;\n";
            out << "    }\n";
            return;
        }
    }
    if (node->to == PrimitiveKind::BYTE) {
        out << "    stack[sp].i = (jint)(jbyte)stack[sp]" << fromAcc << ";\n";
    }
    else if (node->to == PrimitiveKind::CHAR) {
        out << "    stack[sp].i = (jint)(jchar)stack[sp]" << fromAcc << ";\n";
    }
    else if (node->to == PrimitiveKind::SHORT) {
        out << "    stack[sp].i = (jint)(jshort)stack[sp]" << fromAcc << ";\n";
    }
    else {
        out << "    stack[sp]" << toAcc << " = (" << primitiveToJNIType(node->to) << ")stack[sp]"
            << fromAcc << ";\n";
    }
}

void StackCBackend::Impl::emitUnaryOp(const MIRUnaryOp* node, std::ostringstream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* tgt = "stack[sp]";
    if (node->type == PrimitiveKind::INT) {
        // ~x + 1 = -x（二补码环绕，避免一元负号在 INT_MIN 上的 UB）。
        out << "    " << tgt << acc << " = (jint)(~(uint32_t)" << tgt << acc << " + 1u);\n";
    }
    else if (node->type == PrimitiveKind::LONG) {
        out << "    " << tgt << acc << " = (jlong)(~(uint64_t)" << tgt << acc << " + 1ULL);\n";
    }
    else {
        out << "    " << tgt << acc << " = -" << tgt << acc << ";\n";
    }
}

void StackCBackend::Impl::emitBranch(const MIRBranch* node, std::ostringstream& out)
{
    const char* acc = stackAccessor(node->type);
    out << "    {\n";
    out << "        int cond = (stack[sp - 1]" << acc << " " << cmpOpToC(node->op) << " stack[sp]"
        << acc << ");\n";
    out << "        sp -= 2;\n";
    out << "        if (cond) goto " << labelIdent(node->targetLabel) << ";\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitBranchZero(const MIRBranchZero* node, std::ostringstream& out)
{
    const char* acc  = stackAccessor(node->type);
    const char* zero = (node->type == PrimitiveKind::OBJECT) ? "NULL" : "0";
    out << "    {\n";
    out << "        int cond = (stack[sp]" << acc << " " << cmpOpToC(node->op) << " " << zero
        << ");\n";
    out << "        sp--;\n";
    out << "        if (cond) goto " << labelIdent(node->targetLabel) << ";\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitUncondBranch(const MIRUncondBranch* node, std::ostringstream& out)
{
    out << "    goto " << labelIdent(node->targetLabel) << ";\n";
}

void StackCBackend::Impl::emitSwitch(const MIRSwitch* node, std::ostringstream& out)
{
    out << "    switch (stack[sp--].i) {\n";
    for (auto& c : node->cases) {
        out << "    case " << c.key << ": goto " << labelIdent(c.targetLabel) << ";\n";
    }
    out << "    default: goto " << labelIdent(node->defaultLabel) << ";\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitReturnValue(const MIRReturnValue* node, std::ostringstream& out)
{
    if (node->type == PrimitiveKind::OBJECT) {
        out << "    {\n";
        out << "    jobject _mj2x_ret = stack[sp--].l;\n";
        out << "    _mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, _mj2x_ret);\n";
        out << "    return _mj2x_ret;\n";
        out << "    }\n";
        return;
    }
    out << "    {\n";
    out << "    " << primitiveToJNIType(node->type) << " _mj2x_ret = stack[sp--]"
        << stackAccessor(node->type) << ";\n";
    out << "    _mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, NULL);\n";
    out << "    return _mj2x_ret;\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitNewObject(const MIRNewObject* node, std::ostringstream& out)
{
    // NEW 只分配对象、压栈；构造由后续 INVOKESPECIAL <init>（走 emitInvoke 的
    // CallNonvirtualVoidMethod 路径）完成。用 AllocObject 解耦分配与初始化，
    // 避免 emitNewObject 推断构造器签名（MIRNewObject 不带 ctor desc）。
    out << "    {\n";
    out << "        jclass lc = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->typeDesc + "\")")
        << ";\n";
    out << "        stack[++sp].l = " << trackLocalRefExpr("(*env)->AllocObject(env, lc)") << ";\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitNewArray(const MIRNewArray* node, std::ostringstream& out)
{
    if (node->componentType == PrimitiveKind::OBJECT) {
        out << "    stack[sp].l = (*env)->NewObjectArray(env, stack[sp].i, ";
        if (node->typeDesc.empty())
            out << "(jclass)"
                << trackLocalRefExpr(
                       "_mj2x_find_class(env, _mj2x_owner_class, \"java/lang/Object\")");
        else
            out << classExprForDescriptor(node->typeDesc);
        out << ", NULL);\n";
        out << "    _mj2x_track_local_ref(env, _mj2x_refs, &_mj2x_ref_count, stack[sp].l);\n";
    }
    else {
        out << "    stack[sp].l = "
            << trackLocalRefExpr("(jobject)(*env)->" +
                                 std::string(primitiveToNewArrayFn(node->componentType)) +
                                 "(env, stack[sp].i)")
            << ";\n";
    }
}

void StackCBackend::Impl::emitMultiNewArray(const MIRMultiNewArray* node, std::ostringstream& out)
{
    out << "    {\n";
    out << "        jint dims[" << node->dims << "];\n";
    for (int i = node->dims - 1; i >= 0; --i) {
        out << "        dims[" << i << "] = stack[sp--].i;\n";
    }
    out << "        stack[++sp].l = "
        << trackLocalRefExpr("_mj2x_new_multi_array(env, _mj2x_owner_class, \"" + node->typeDesc +
                             "\", dims, " + std::to_string(node->dims) + ")")
        << ";\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitArrayLength(const MIRArrayLength* node, std::ostringstream& out)
{
    (void)node;
    emitNullPointerTrap("stack[sp].l", out);
    out << renderStackCSnippet(StackCSnippet::ArrayLength, {});
}

void StackCBackend::Impl::emitArrayLoad(const MIRArrayLoad* node, std::ostringstream& out)
{
    emitNullPointerTrap("stack[sp - 1].l", out);
    if (node->componentType == PrimitiveKind::OBJECT) {
        out << renderStackCSnippet(
            StackCSnippet::ObjectArrayLoad,
            {{"value",
              trackLocalRefExpr("(*env)->GetObjectArrayElement(env, (jobjectArray)stack[sp - 1].l, "
                                "stack[sp].i)")}});
        return;
    }
    if (node->componentType == PrimitiveKind::BYTE) {
        out << "    {\n";
        out << "        jclass boolean_array_class = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"[Z\")") << ";\n";
        out << "        if ((*env)->IsInstanceOf(env, stack[sp - 1].l, boolean_array_class)) {\n";
        out << "            jboolean value;\n";
        out << "            (*env)->GetBooleanArrayRegion(env, (jbooleanArray)stack[sp - 1].l, "
               "stack[sp].i, 1, &value);\n";
        out << "            stack[sp - 1].i = (jint)value;\n";
        out << "        } else {\n";
        out << "            jbyte value;\n";
        out << "            (*env)->GetByteArrayRegion(env, (jbyteArray)stack[sp - 1].l, "
               "stack[sp].i, 1, &value);\n";
        out << "            stack[sp - 1].i = (jint)value;\n";
        out << "        }\n";
        out << "    }\n";
        out << "    sp--;\n";
        return;
    }
    const char* suffix = primitiveToJNISuffix(node->componentType);
    const char* type   = primitiveToJNIType(node->componentType);
    out << renderStackCSnippet(
        StackCSnippet::PrimitiveArrayLoad,
        {{"type", type},
         {"suffix", suffix},
         {"array_type", primitiveToJNIArrayType(node->componentType)},
         {"acc", stackValueAccessor(node->componentType)},
         {"target_type", narrowOnOperandStack(node->componentType) ? "jint" : type}});
}

void StackCBackend::Impl::emitArrayStore(const MIRArrayStore* node, std::ostringstream& out)
{
    emitNullPointerTrap("stack[sp - 2].l", out);
    if (node->componentType == PrimitiveKind::OBJECT) {
        out << renderStackCSnippet(StackCSnippet::ObjectArrayStore, {});
        return;
    }
    if (node->componentType == PrimitiveKind::BYTE) {
        out << "    {\n";
        out << "        jclass boolean_array_class = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"[Z\")") << ";\n";
        out << "        if ((*env)->IsInstanceOf(env, stack[sp - 2].l, boolean_array_class)) {\n";
        out << "            jboolean value = (jboolean)(stack[sp].i & 1);\n";
        out << "            (*env)->SetBooleanArrayRegion(env, (jbooleanArray)stack[sp - 2].l, "
               "stack[sp - 1].i, 1, &value);\n";
        out << "        } else {\n";
        out << "            jbyte value = (jbyte)stack[sp].i;\n";
        out << "            (*env)->SetByteArrayRegion(env, (jbyteArray)stack[sp - 2].l, stack[sp "
               "- 1].i, 1, &value);\n";
        out << "        }\n";
        out << "    }\n";
        out << "    sp -= 3;\n";
        return;
    }
    const char* suffix = primitiveToJNISuffix(node->componentType);
    const char* type   = primitiveToJNIType(node->componentType);
    out << renderStackCSnippet(StackCSnippet::PrimitiveArrayStore,
                               {{"type", type},
                                {"suffix", suffix},
                                {"array_type", primitiveToJNIArrayType(node->componentType)},
                                {"acc", stackValueAccessor(node->componentType)}});
}

void StackCBackend::Impl::emitFieldGet(const MIRFieldGet* node, std::ostringstream& out)
{
    auto        pk      = jvmDescToPrimitive(node->desc);
    const char* suffix  = primitiveToJNISuffix(pk);
    const char* acc     = stackValueAccessor(pk);
    const auto fidCache = cacheIdentifier("fid", node->owner + "." + node->name + ":" + node->desc);
    out << "    {\n";
    out << "        jclass lc = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner + "\")")
        << ";\n";
    emitFieldIdCache(fidCache, "fid_local", "lc", node->name, node->desc, false, out);
    emitNullPointerTrap("stack[sp].l", out);
    out << "        stack[sp]" << acc << " = ("
        << (narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk)) << ")(*env)->Get" << suffix
        << "Field(env, stack[sp].l, fid_local);\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitFieldPut(const MIRFieldPut* node, std::ostringstream& out)
{
    auto        pk      = jvmDescToPrimitive(node->desc);
    const char* suffix  = primitiveToJNISuffix(pk);
    const char* acc     = stackValueAccessor(pk);
    const auto fidCache = cacheIdentifier("fid", node->owner + "." + node->name + ":" + node->desc);
    out << "    {\n";
    out << "        jclass lc = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner + "\")")
        << ";\n";
    emitFieldIdCache(fidCache, "fid_local", "lc", node->name, node->desc, false, out);
    emitNullPointerTrap("stack[sp - 1].l", out);
    out << "        (*env)->Set" << suffix << "Field(env, stack[sp - 1].l, fid_local, ("
        << primitiveToJNIType(pk) << ")stack[sp]" << acc << ");\n";
    out << "    }\n";
    out << "    sp -= 2;\n";
}

void StackCBackend::Impl::emitStaticGet(const MIRStaticGet* node, std::ostringstream& out)
{
    auto        pk     = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc    = stackValueAccessor(pk);
    const auto  fidCache =
        cacheIdentifier("sfid", node->owner + "." + node->name + ":" + node->desc);
    out << "    {\n";
    out << "        jclass lc = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner + "\")")
        << ";\n";
    emitFieldIdCache(fidCache, "fid_local", "lc", node->name, node->desc, true, out);
    out << "        stack[++sp]" << acc << " = ("
        << (narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk)) << ")(*env)->GetStatic"
        << suffix << "Field(env, lc, fid_local);\n";
    out << "    }\n";
}

void StackCBackend::Impl::emitStaticPut(const MIRStaticPut* node, std::ostringstream& out)
{
    auto        pk     = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc    = stackValueAccessor(pk);
    const auto  fidCache =
        cacheIdentifier("sfid", node->owner + "." + node->name + ":" + node->desc);
    out << "    {\n";
    out << "        jclass lc = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner + "\")")
        << ";\n";
    emitFieldIdCache(fidCache, "fid_local", "lc", node->name, node->desc, true, out);
    out << "        (*env)->SetStatic" << suffix << "Field(env, lc, fid_local, ("
        << primitiveToJNIType(pk) << ")stack[sp]" << acc << ");\n";
    out << "    }\n";
    out << "    sp--;\n";
}

void StackCBackend::Impl::emitInvoke(const MIRInvoke* node, std::ostringstream& out)
{
    ca::str::Utf8StringRef descRef = ca::str::Utf8StringRef::from_cstr(node->desc.c_str());
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor().to_std_string());

    // 调用类别：INVOKESTATIC → CallStatic*；INVOKESPECIAL → CallNonvirtual*
    //   （含构造器 <init>：CallNonvirtualVoidMethod 作用于 AllocObject 出的对象）；
    //   INVOKEVIRTUAL/INVOKEINTERFACE → Call*Method。
    const char* callPrefix   = "Call";
    bool        isStatic     = (node->invokeOpcode == Opcodes::INVOKESTATIC);
    bool        isNonvirtual = (node->invokeOpcode == Opcodes::INVOKESPECIAL);
    if (isStatic) {
        callPrefix = "CallStatic";
    }
    else if (isNonvirtual) {
        callPrefix = "CallNonvirtual";
    }
    // 返回类型 → JNI 后缀（void 用 "Void"）。
    const char* retSuffix =
        (retType == PrimitiveKind::VOID) ? "Void" : primitiveToJNISuffix(retType);

    // 参数：运行时 stack[] 每个 JVM 值占一个 union jvalue；locals 仍按 JVM slot 编号。
    // 因此 invoke 的栈距离按参数个数算，不按 long/double 的 JVM slot 宽度算。
    auto        argTypes    = Type::getArgumentTypes(descRef);
    int         argCount    = static_cast<int>(argTypes.size());
    std::string recvExpr    = "sp - " + std::to_string(argCount);
    int         resultIndex = isStatic ? (argCount == 0 ? -1 : argCount - 1) : argCount;
    const auto  methodCache = cacheIdentifier(isStatic ? "smid" : "mid",
                                             node->owner + "." + node->name + ":" + node->desc +
                                                 ":" + std::to_string(node->invokeOpcode));

    // 整段放 block 内：先查 jclass/jmethodID，再调用。返回值写入接收者槽（非 void）。
    out << "    {\n";
    if (isStatic) {
        out << "        jclass lc = (jclass)"
            << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner +
                                 "\")")
            << ";\n";
        emitMethodIdCache(methodCache, "mid_local", "lc", node->name, node->desc, true, out);
        out << "        ";
        if (retType != PrimitiveKind::VOID) {
            if (argCount == 0)
                out << "stack[++sp]" << stackValueAccessor(retType) << " = ";
            else
                out << "stack[sp - " << resultIndex << "]" << stackValueAccessor(retType) << " = ";
        }
        out << "(*env)->" << callPrefix << retSuffix << "Method(env, lc, mid_local";
    }
    else {
        out << "        jclass lc = (jclass)"
            << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + node->owner +
                                 "\")")
            << ";\n";
        emitMethodIdCache(methodCache, "mid_local", "lc", node->name, node->desc, false, out);
        emitNullPointerTrap("stack[" + recvExpr + "].l", out);
        out << "        ";
        if (retType != PrimitiveKind::VOID)
            out << "stack[" << recvExpr << "]" << stackValueAccessor(retType) << " = ";
        out << "(*env)->" << callPrefix << retSuffix << "Method(env, stack[" << recvExpr << "].l";
        // CallNonvirtual* 需要 (env, obj, jclass, methodID, ...)：在 obj 后插 jclass lc。
        if (isNonvirtual) out << ", lc";
        out << ", mid_local";
    }
    // 参数列表：arg0 在 sp-argCount+1，依次往后；静态/实例共享同一参数区。
    for (int i = 0; i < argCount; ++i) {
        auto        pka        = jvmSortToPrimitive(argTypes[static_cast<size_t>(i)].getSort());
        int         distFromSp = argCount - 1 - i;
        std::string argExpr =
            "stack[sp - " + std::to_string(distFromSp) + "]" + stackValueAccessor(pka);
        out << ", " << argExpr;
    }
    out << ");\n";
    emitPendingExceptionCheck(out, 8);
    out << "    }\n";

    if (isStatic) {
        if (retType == PrimitiveKind::VOID) {
            if (argCount > 0) out << "    sp -= " << argCount << ";\n";
        }
        else if (argCount > 0) {
            out << "    sp -= " << (argCount - 1) << ";\n";
        }
    }
    else {
        int popCount = argCount + 1;
        if (retType != PrimitiveKind::VOID) popCount -= 1;
        if (popCount > 0) out << "    sp -= " << popCount << ";\n";
    }
}

void StackCBackend::Impl::emitInvokeDynamic(const MIRInvokeDynamic* node, std::ostringstream& out)
{
    ca::str::Utf8StringRef descRef  = ca::str::Utf8StringRef::from_cstr(node->desc.c_str());
    auto                   argTypes = Type::getArgumentTypes(descRef);
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor().to_std_string());
    const int argCount = static_cast<int>(argTypes.size());

    auto emitExceptionCheck = [&]() {
        out << "        if ((*env)->ExceptionCheck(env)) {\n";
        if (!currentExceptionDispatchTarget.empty()) {
            out << "            goto " << currentExceptionDispatchTarget << ";\n";
        }
        else {
            emitExceptionReturn(currentMethodDesc, out, 12);
        }
        out << "        }\n";
    };

    auto emitStringObject = [&](const std::string& varName, const std::string& value) {
        out << "        jstring " << varName << " = (jstring)"
            << trackLocalRefExpr("(*env)->NewStringUTF(env, " + cStringLiteral(value) + ")")
            << ";\n";
    };

    auto emitClassObjectForDescriptor = [&](const std::string& varName, const std::string& desc) {
        const auto kind = jvmDescToPrimitive(desc);
        if (kind != PrimitiveKind::OBJECT || desc == "V") {
            out << "        jclass " << varName << "_wrapper = (jclass)"
                << trackLocalRefExpr("(*env)->FindClass(env, \"" +
                                     std::string(boxedClassName(kind)) + "\")")
                << ";\n";
            emitCLine(out,
                      2,
                      "jfieldID " + varName + "_type_fid = (*env)->GetStaticFieldID(env, " +
                          varName + "_wrapper, \"TYPE\", \"Ljava/lang/Class;\");");
            out << "        jclass " << varName << " = (jclass)"
                << trackLocalRefExpr("(*env)->GetStaticObjectField(env, " + varName + "_wrapper, " +
                                     varName + "_type_fid)")
                << ";\n";
        }
        else {
            out << "        jclass " << varName << " = " << classExprForDescriptor(desc) << ";\n";
        }
    };

    auto emitMethodType = [&](const std::string& varName, const std::string& methodDesc) {
        emitStringObject(varName + "_desc", methodDesc);
        out << "        jclass " << varName << "_class_cls = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/Class\")") << ";\n";
        emitCLine(out,
                  2,
                  "jmethodID " + varName + "_loader_mid = (*env)->GetMethodID(env, " + varName +
                      "_class_cls, \"getClassLoader\", \"()Ljava/lang/ClassLoader;\");");
        out << "        jobject " << varName << "_loader = "
            << trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_owner_class, " + varName +
                                 "_loader_mid)")
            << ";\n";
        emitExceptionCheck();
        out << "        jclass " << varName << "_mt_cls = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodType\")")
            << ";\n";
        emitCLine(
            out,
            2,
            "jmethodID " + varName + "_from_desc_mid = (*env)->GetStaticMethodID(env, " + varName +
                "_mt_cls, \"fromMethodDescriptorString\", "
                "\"(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;\");");
        out << "        jobject " << varName << " = "
            << trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, " + varName + "_mt_cls, " +
                                 varName + "_from_desc_mid, " + varName + "_desc, " + varName +
                                 "_loader)")
            << ";\n";
        emitExceptionCheck();
    };

    auto emitBoxPrimitiveExpr =
        [&](const std::string& varName, PrimitiveKind kind, const std::string& valueExpr) {
            out << "        jclass " << varName << "_box_cls = (jclass)"
                << trackLocalRefExpr("(*env)->FindClass(env, \"" +
                                     std::string(boxedClassName(kind)) + "\")")
                << ";\n";
            emitCLine(out,
                      2,
                      "jmethodID " + varName + "_valueof_mid = (*env)->GetStaticMethodID(env, " +
                          varName + "_box_cls, \"valueOf\", " +
                          cStringLiteral(boxedValueOfDesc(kind)) + ");");
            out << "        jobject " << varName << " = "
                << trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, " + varName +
                                     "_box_cls, " + varName + "_valueof_mid, " + valueExpr + ")")
                << ";\n";
        };

    auto emitBoxStackValue =
        [&](const std::string& varName, PrimitiveKind kind, const std::string& stackIndexExpr) {
            if (kind == PrimitiveKind::OBJECT) {
                out << "        jobject " << varName << " = stack[" << stackIndexExpr << "].l;\n";
                return;
            }
            std::string valueExpr = "(" + std::string(primitiveToJNIType(kind)) + ")stack[" +
                                    stackIndexExpr + "]" + stackValueAccessor(kind);
            emitBoxPrimitiveExpr(varName, kind, valueExpr);
        };

    auto emitMethodHandle = [&](const std::string& varName, const Handle& handle) {
        const int         tag   = handle.getTag();
        const std::string owner = utf8RefToString(handle.getOwner());
        const std::string name  = utf8RefToString(handle.getName());
        const std::string desc  = utf8RefToString(handle.getDesc());
        out << "        jobject " << varName << " = NULL;\n";
        out << "        {\n";
        out << "            jclass " << varName << "_lookup_cls = (jclass)"
            << trackLocalRefExpr(
                   "(*env)->FindClass(env, \"java/lang/invoke/MethodHandles$Lookup\")")
            << ";\n";
        out << "            jclass " << varName << "_owner_cls = (jclass)"
            << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + owner + "\")")
            << ";\n";
        if (tag == Opcodes::H_GETFIELD || tag == Opcodes::H_GETSTATIC ||
            tag == Opcodes::H_PUTFIELD || tag == Opcodes::H_PUTSTATIC) {
            emitStringObject(varName + "_name", name);
            emitClassObjectForDescriptor(varName + "_field_type", desc);
        }
        else {
            if (tag != Opcodes::H_NEWINVOKESPECIAL) {
                emitStringObject(varName + "_name", name);
            }
            emitMethodType(varName + "_type", desc);
        }

        const char* findName = "findVirtual";
        const char* findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
                               "Ljava/lang/invoke/MethodHandle;";
        std::string args     = varName + "_owner_cls";
        switch (tag) {
        case Opcodes::H_GETFIELD:
            findName = "findGetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_name, " + varName + "_field_type";
            break;
        case Opcodes::H_GETSTATIC:
            findName = "findStaticGetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_name, " + varName + "_field_type";
            break;
        case Opcodes::H_PUTFIELD:
            findName = "findSetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_name, " + varName + "_field_type";
            break;
        case Opcodes::H_PUTSTATIC:
            findName = "findStaticSetter";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_name, " + varName + "_field_type";
            break;
        case Opcodes::H_INVOKESTATIC:
            findName = "findStatic";
            args += ", " + varName + "_name, " + varName + "_type";
            break;
        case Opcodes::H_INVOKESPECIAL:
            findName = "findSpecial";
            findDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/"
                       "lang/Class;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_name, " + varName + "_type, _mj2x_owner_class";
            break;
        case Opcodes::H_NEWINVOKESPECIAL:
            findName = "findConstructor";
            findDesc = "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
                       "Ljava/lang/invoke/MethodHandle;";
            args += ", " + varName + "_type";
            break;
        case Opcodes::H_INVOKEINTERFACE:
        case Opcodes::H_INVOKEVIRTUAL:
        default: args += ", " + varName + "_name, " + varName + "_type"; break;
        }
        emitCLine(out,
                  3,
                  "jmethodID " + varName + "_find_mid = (*env)->GetMethodID(env, " + varName +
                      "_lookup_cls, " + cStringLiteral(findName) + ", " + cStringLiteral(findDesc) +
                      ");");
        out << "            " << varName << " = "
            << trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_indy_lookup, " + varName +
                                 "_find_mid, " + args + ")")
            << ";\n";
        emitExceptionCheck();
        out << "        }\n";
    };

    auto emitAnyAsObject = [&](const std::string& varName, const ca::core::Any& value) {
        if (auto* p = value.as<std::string>()) {
            emitStringObject(varName, *p);
        }
        else if (auto* p = value.as<ca::str::Utf8StringRef>()) {
            emitStringObject(varName, p->to_std_string());
        }
        else if (auto* p = value.as<Type>()) {
            if (p->getSort() == Type::METHOD) {
                emitMethodType(varName, methodTypeDescriptorString(*p));
            }
            else {
                emitClassObjectForDescriptor(varName, typeDescriptorString(*p));
            }
        }
        else if (auto* p = value.as<Handle>()) {
            emitMethodHandle(varName, *p);
        }
        else if (auto* p = value.as<ca::i32>()) {
            emitBoxPrimitiveExpr(varName, PrimitiveKind::INT, "(jint)" + std::to_string(*p));
        }
        else if (auto* p = value.as<ca::i64>()) {
            emitBoxPrimitiveExpr(
                varName, PrimitiveKind::LONG, "(jlong)" + std::to_string(*p) + "LL");
        }
        else if (auto* p = value.as<float>()) {
            std::ostringstream valueText;
            valueText << std::setprecision(9) << *p << "f";
            emitBoxPrimitiveExpr(varName, PrimitiveKind::FLOAT, valueText.str());
        }
        else if (auto* p = value.as<double>()) {
            std::ostringstream valueText;
            valueText << std::setprecision(17) << *p;
            emitBoxPrimitiveExpr(varName, PrimitiveKind::DOUBLE, valueText.str());
        }
        else {
            out << "        jobject " << varName << " = NULL;\n";
        }
    };

    auto emitAnyForFormal = [&](const std::string&   varName,
                                const Type&          formal,
                                const ca::core::Any& value) -> std::string {
        const int sort = formal.getSort();
        if (sort == Type::OBJECT || sort == Type::ARRAY) {
            emitAnyAsObject(varName, value);
            return varName;
        }
        auto primitive = jvmSortToPrimitive(sort);
        if (auto* p = value.as<ca::i32>()) {
            return "(jint)" + std::to_string(*p);
        }
        if (auto* p = value.as<ca::i64>()) {
            return "(jlong)" + std::to_string(*p) + "LL";
        }
        if (auto* p = value.as<float>()) {
            std::ostringstream valueText;
            valueText << std::setprecision(9) << *p << "f";
            return valueText.str();
        }
        if (auto* p = value.as<double>()) {
            std::ostringstream valueText;
            valueText << std::setprecision(17) << *p;
            return valueText.str();
        }
        if (primitive == PrimitiveKind::BOOLEAN) return "JNI_FALSE";
        return "0";
    };

    out << "    {\n";
    out << "        jclass _mj2x_indy_object_cls = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/Object\")") << ";\n";
    out << "        jobjectArray _mj2x_indy_args = (jobjectArray)"
        << trackLocalRefExpr("(*env)->NewObjectArray(env, " + std::to_string(argCount) +
                             ", _mj2x_indy_object_cls, NULL)")
        << ";\n";
    for (int i = 0; i < argCount; ++i) {
        const auto        kind = jvmSortToPrimitive(argTypes[static_cast<size_t>(i)].getSort());
        const int         distFromSp = argCount - 1 - i;
        const std::string stackIndexExpr =
            distFromSp == 0 ? "sp" : "sp - " + std::to_string(distFromSp);
        emitBoxStackValue("_mj2x_indy_arg_" + std::to_string(i), kind, stackIndexExpr);
        out << "        (*env)->SetObjectArrayElement(env, _mj2x_indy_args, " << i
            << ", _mj2x_indy_arg_" << i << ");\n";
    }
    if (argCount > 0) {
        out << "        sp -= " << argCount << ";\n";
    }

    out << "        jclass _mj2x_indy_mh_cls = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodHandles\")") << ";\n";
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_lookup_mid = (*env)->GetStaticMethodID(env, "
              "_mj2x_indy_mh_cls, \"lookup\", \"()Ljava/lang/invoke/MethodHandles$Lookup;\");");
    out << "        jobject _mj2x_indy_base_lookup = "
        << trackLocalRefExpr(
               "(*env)->CallStaticObjectMethod(env, _mj2x_indy_mh_cls, _mj2x_indy_lookup_mid)")
        << ";\n";
    emitExceptionCheck();
    out << "        jobject _mj2x_indy_lookup = _mj2x_indy_base_lookup;\n";
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_private_lookup_mid = (*env)->GetStaticMethodID(env, "
              "_mj2x_indy_mh_cls, \"privateLookupIn\", "
              "\"(Ljava/lang/Class;Ljava/lang/invoke/MethodHandles$Lookup;)Ljava/lang/invoke/"
              "MethodHandles$Lookup;\");");
    out << "        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);\n";
    out << "        if (_mj2x_indy_private_lookup_mid != NULL) {\n";
    out << "            jobject _mj2x_indy_private_lookup = "
        << trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, _mj2x_indy_mh_cls, "
                             "_mj2x_indy_private_lookup_mid, _mj2x_owner_class, "
                             "_mj2x_indy_base_lookup)")
        << ";\n";
    out << "            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);\n";
    out << "            else _mj2x_indy_lookup = _mj2x_indy_private_lookup;\n";
    out << "        }\n";

    emitStringObject("_mj2x_indy_name", node->name);
    emitMethodType("_mj2x_indy_type", node->desc);

    if (!node->bsm.has_value()) {
        emitUnsupported("invokedynamic without bootstrap metadata", out);
        out << "    }\n";
        return;
    }
    const auto&       bsm      = *node->bsm;
    const std::string bsmOwner = utf8RefToString(bsm.getOwner());
    const std::string bsmName  = utf8RefToString(bsm.getName());
    const std::string bsmDesc  = utf8RefToString(bsm.getDesc());
    const auto        indyTargetCache =
        cacheIdentifier("indy_target",
                        currentClassName + "." + currentMethodName + currentMethodDesc + ":" +
                            node->name + ":" + node->desc + ":" + bsmOwner + "." + bsmName + ":" +
                            bsmDesc + ":" + std::to_string(node->bsmArgs.size()));

    out << "        static jobject " << indyTargetCache << " = NULL;\n";
    out << "        jobject _mj2x_indy_target = NULL;\n";
    out << "        if (" << indyTargetCache << " != NULL) {\n";
    out << "            _mj2x_indy_target = (*env)->NewLocalRef(env, " << indyTargetCache << ");\n";
    out << "            if (_mj2x_indy_target == NULL) " << indyTargetCache << " = NULL;\n";
    out << "        }\n";
    out << "        if (_mj2x_indy_target == NULL) {\n";
    auto bsmArgTypes = Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(bsmDesc.c_str()));
    out << "        jclass _mj2x_indy_bsm_cls = (jclass)"
        << trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + bsmOwner + "\")")
        << ";\n";
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_bsm_mid = (*env)->GetStaticMethodID(env, "
              "_mj2x_indy_bsm_cls, " +
                  cStringLiteral(bsmName) + ", " + cStringLiteral(bsmDesc) + ");");

    std::vector<std::string> bsmExprs;
    size_t                   bsmArgIndex = 0;
    for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
        if (i == 0) {
            bsmExprs.push_back("_mj2x_indy_lookup");
            continue;
        }
        if (i == 1) {
            bsmExprs.push_back("_mj2x_indy_name");
            continue;
        }
        if (i == 2) {
            bsmExprs.push_back("_mj2x_indy_type");
            continue;
        }
        const auto& formal                = bsmArgTypes[i];
        const bool  isTrailingObjectArray = i + 1 == bsmArgTypes.size() &&
                                           formal.getSort() == Type::ARRAY &&
                                           typeDescriptorString(formal) == "[Ljava/lang/Object;";
        if (isTrailingObjectArray) {
            const size_t remaining =
                bsmArgIndex < node->bsmArgs.size() ? node->bsmArgs.size() - bsmArgIndex : 0;
            out << "        jobjectArray _mj2x_indy_bsm_varargs = (jobjectArray)"
                << trackLocalRefExpr("(*env)->NewObjectArray(env, " + std::to_string(remaining) +
                                     ", _mj2x_indy_object_cls, NULL)")
                << ";\n";
            for (size_t j = 0; j < remaining; ++j) {
                const std::string argVar = "_mj2x_indy_bsm_vararg_" + std::to_string(j);
                emitAnyAsObject(argVar, node->bsmArgs[bsmArgIndex + j]);
                out << "        (*env)->SetObjectArrayElement(env, _mj2x_indy_bsm_varargs, " << j
                    << ", " << argVar << ");\n";
            }
            bsmArgIndex = node->bsmArgs.size();
            bsmExprs.push_back("_mj2x_indy_bsm_varargs");
            continue;
        }
        if (bsmArgIndex >= node->bsmArgs.size()) {
            bsmExprs.push_back("NULL");
            continue;
        }
        const std::string argVar = "_mj2x_indy_bsm_arg_" + std::to_string(bsmArgIndex);
        bsmExprs.push_back(emitAnyForFormal(argVar, formal, node->bsmArgs[bsmArgIndex]));
        ++bsmArgIndex;
    }

    std::string bsmCall =
        "(*env)->CallStaticObjectMethod(env, _mj2x_indy_bsm_cls, _mj2x_indy_bsm_mid";
    for (const auto& expr : bsmExprs) {
        bsmCall += ", ";
        bsmCall += expr;
    }
    bsmCall += ")";
    out << "        jobject _mj2x_indy_callsite = " << trackLocalRefExpr(bsmCall) << ";\n";
    emitExceptionCheck();

    out << "        jclass _mj2x_indy_callsite_cls = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/CallSite\")") << ";\n";
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_get_target_mid = (*env)->GetMethodID(env, "
              "_mj2x_indy_callsite_cls, \"getTarget\", \"()Ljava/lang/invoke/MethodHandle;\");");
    out << "            _mj2x_indy_target = "
        << trackLocalRefExpr(
               "(*env)->CallObjectMethod(env, _mj2x_indy_callsite, _mj2x_indy_get_target_mid)")
        << ";\n";
    emitExceptionCheck();
    out << "            if (_mj2x_indy_target != NULL) {\n";
    out << "                jobject _mj2x_indy_target_global = (*env)->NewGlobalRef(env, "
           "_mj2x_indy_target);\n";
    out << "                if (_mj2x_indy_target_global != NULL) " << indyTargetCache
        << " = _mj2x_indy_target_global;\n";
    out << "            }\n";
    out << "        }\n";

    out << "        jclass _mj2x_indy_method_handle_cls = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodHandle\")") << ";\n";
    const bool useOneObjectInvokeHelper =
        retType == PrimitiveKind::OBJECT && argCount == 1 &&
        (argTypes[0].getSort() == Type::OBJECT || argTypes[0].getSort() == Type::ARRAY);
    if (useOneObjectInvokeHelper) {
        out << "        jclass _mj2x_indy_cloader_cls = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"mj2x/cloader/Loader\")") << ";\n";
        emitCLine(out,
                  2,
                  "jmethodID _mj2x_indy_helper_mid = (*env)->GetStaticMethodID(env, "
                  "_mj2x_indy_cloader_cls, \"invokeMethodHandle1\", "
                  "\"(Ljava/lang/invoke/MethodHandle;Ljava/lang/Object;)Ljava/lang/Object;\");");
        out << "        jobject _mj2x_indy_capture0 = stack[sp + 1].l;\n";
        out << "        stack[++sp].l = "
            << trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, _mj2x_indy_cloader_cls, "
                                 "_mj2x_indy_helper_mid, _mj2x_indy_target, _mj2x_indy_capture0)")
            << ";\n";
        emitExceptionCheck();
        out << "    }\n";
        return;
    }

    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_invoke_mid = (*env)->GetMethodID(env, "
              "_mj2x_indy_method_handle_cls, \"invokeWithArguments\", "
              "\"([Ljava/lang/Object;)Ljava/lang/Object;\");");
    out << "        jobject _mj2x_indy_result = "
        << trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_indy_target, "
                             "_mj2x_indy_invoke_mid, _mj2x_indy_args)")
        << ";\n";
    emitExceptionCheck();

    if (retType == PrimitiveKind::VOID) {
        out << "        (void)_mj2x_indy_result;\n";
    }
    else if (retType == PrimitiveKind::OBJECT) {
        out << "        stack[++sp].l = _mj2x_indy_result;\n";
    }
    else {
        out << "        jclass _mj2x_indy_ret_box_cls = (jclass)"
            << trackLocalRefExpr("(*env)->FindClass(env, \"" +
                                 std::string(boxedClassName(retType)) + "\")")
            << ";\n";
        emitCLine(out,
                  2,
                  "jmethodID _mj2x_indy_unbox_mid = (*env)->GetMethodID(env, "
                  "_mj2x_indy_ret_box_cls, " +
                      cStringLiteral(boxedUnboxMethod(retType)) + ", " +
                      cStringLiteral(boxedUnboxDesc(retType)) + ");");
        out << "        stack[++sp]" << stackValueAccessor(retType) << " = ("
            << (narrowOnOperandStack(retType) ? "jint" : primitiveToJNIType(retType))
            << ")(*env)->Call" << primitiveToJNISuffix(retType)
            << "Method(env, _mj2x_indy_result, _mj2x_indy_unbox_mid);\n";
    }
    out << "    }\n";
}

void StackCBackend::Impl::emitCheckCast(const MIRCheckCast* node, std::ostringstream& out)
{
    out << "    if (stack[sp].l != NULL && !(*env)->IsInstanceOf(env, stack[sp].l, "
        << classExprForDescriptor(node->typeDesc) << ")) {\n";
    out << "        jclass ex_class = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/ClassCastException\")") << ";\n";
    out << "        (*env)->ThrowNew(env, ex_class, \"" << node->typeDesc << "\");\n";
    if (!currentExceptionDispatchTarget.empty())
        out << "        goto " << currentExceptionDispatchTarget << ";\n";
    else
        emitExceptionReturn(currentMethodDesc, out, 8);
    out << "    }\n";
}

void StackCBackend::Impl::emitUnsupported(const char* what, std::ostringstream& out)
{
    out << "    jclass ex_class = (jclass)"
        << trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/UnsupportedOperationException\")")
        << ";\n";
    out << "    (*env)->ThrowNew(env, ex_class, \"" << what << "\");\n";
    if (!currentExceptionDispatchTarget.empty())
        out << "    goto " << currentExceptionDispatchTarget << ";\n";
    else
        emitExceptionReturn(currentMethodDesc, out, 4);
}

void StackCBackend::Impl::emitInstanceOf(const MIRInstanceOf* node, std::ostringstream& out)
{
    out << "    stack[sp].i = (*env)->IsInstanceOf(env, stack[sp].l, "
        << classExprForDescriptor(node->typeDesc) << ");\n";
}

void StackCBackend::Impl::emitMonitor(const MIRMonitor* node, std::ostringstream& out)
{
    if (node->enter)
        out << "    (*env)->MonitorEnter(env, stack[sp--].l);\n";
    else
        out << "    (*env)->MonitorExit(env, stack[sp--].l);\n";
}

void StackCBackend::Impl::emitThrow(const MIRThrow* node, std::ostringstream& out)
{
    (void)node;
    out << "    (*env)->Throw(env, stack[sp--].l);\n";
    if (!currentExceptionDispatchTarget.empty())
        out << "    goto " << currentExceptionDispatchTarget << ";\n";
    else
        emitExceptionReturn(currentMethodDesc, out, 4);
}

}   // namespace mj2x

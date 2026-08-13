/// @file stackc_backend_internal.hpp
/// @brief StackC 后端内部共享定义（跨拆分 cpp 文件可见）。
///
/// @details
/// `stackc_backend.cpp` 原先是一个 3300+ 行的单体：辅助结构、无状态 helper、
/// MIR 发射器、注册逻辑全部挤在一起。本头把「类型/常量/无状态 helper + Impl
/// 完整定义」收口，各拆分文件（stackc_mir_*.cpp / stackc_registration.cpp /
/// stackc_exception.cpp）include 后共享同一套状态与工具，避免重复实现。
///
/// 纯无状态字符串/类型映射 helper 遵循 c_helpers.hpp 的 header-inline 惯例；
/// 大块运行时模板留在 stackc_runtime_prelude.cpp，发射器留在各 cpp。
#pragma once

#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/frontend/mir_node.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/postprocessor/cloader_call.hpp"

#include "mj2x/codegen/code_snippet.hpp"

#include <jbytecode/jbytecode.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;

// ============================================================
// 注册/代理辅助结构
// ============================================================

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

/// RegisterNatives 桥接常量从共享头 mj2x/postprocessor/cloader_call.hpp 取用。
inline constexpr const char* kRegisterBridgeClassName     = cloader::kClassName;
inline constexpr const char* kRegisterBridgeMethodName    = cloader::kRegisterName;
inline constexpr const char* kRegisterBridgeMethodDesc    = cloader::kRegisterDesc;
inline constexpr const char* kRegisterBridgeCFunctionName = cloader::kBridgeCFunc;
inline constexpr const char* kHiddenClinitMethodName      = "__mj2x_hiddenClinit";
inline constexpr const char* kHiddenClinitMethodDesc      = "(Ljava/lang/Class;)V";

// ============================================================
// 无状态 helper（header-inline，对齐 c_helpers.hpp 惯例）
// ============================================================

inline std::string labelIdent(unsigned index)
{
    return "L" + std::to_string(index);
}

inline std::string registerFunctionName(const std::string& className)
{
    std::string ident = "_mj2x_register_";
    for (unsigned char c : className) {
        if (std::isalnum(c)) {
            ident += static_cast<char>(c);
        }
        else {
            ident += "_";
            ident += codegen::hex64(c).substr(14);
        }
    }
    return ident;
}

inline std::string cacheIdentifier(const char* prefix, const std::string& key)
{
    return std::string(prefix) + "_" + codegen::hex64(codegen::fnv1a64(key));
}

inline bool isCategory2Kind(PrimitiveKind kind)
{
    return kind == PrimitiveKind::LONG || kind == PrimitiveKind::DOUBLE;
}

inline bool stackOpTopIsCategory2(const MIRStackOp* op, size_t depth = 0)
{
    return op != nullptr && depth < op->stackBeforeTop.size() &&
           isCategory2Kind(op->stackBeforeTop[depth]);
}

// ---- 类型映射 ----

inline const char* primitiveToJNIType(PrimitiveKind k)
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

inline const char* primitiveToJNISignature(PrimitiveKind k)
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

inline const char* primitiveToJNISuffix(PrimitiveKind k)
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

inline const char* primitiveToJNIArrayType(PrimitiveKind k)
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

inline const char* primitiveToNewArrayFn(PrimitiveKind k)
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

inline const char* stackAccessor(PrimitiveKind k)
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

inline bool narrowOnOperandStack(PrimitiveKind k)
{
    return k == PrimitiveKind::BOOLEAN || k == PrimitiveKind::BYTE || k == PrimitiveKind::CHAR ||
           k == PrimitiveKind::SHORT;
}

inline const char* stackValueAccessor(PrimitiveKind k)
{
    return narrowOnOperandStack(k) ? ".i" : stackAccessor(k);
}

inline const char* castResultAccessor(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::BYTE:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::SHORT: return ".i";
    default: return stackAccessor(k);
    }
}

/// @brief 表达式透传（local-ref 跟踪已移除：对齐 JNIC 裸 JNI 模型，方法返回时
///        JVM 自动释放 local refs，HotSpot 引用表自动扩容）。
inline std::string trackLocalRefExpr(const std::string& expr)
{
    return expr;
}

inline std::string classExprForDescriptor(const std::string& typeDesc,
                                          const std::string& ownerExpr = "_mj2x_owner_class")
{
    if (typeDesc.size() >= 2 && typeDesc.front() == 'L' && typeDesc.back() == ';') {
        return "(jclass)" + trackLocalRefExpr("_mj2x_find_class(env, " + ownerExpr + ", \"" +
                                              typeDesc.substr(1, typeDesc.size() - 2) + "\")");
    }
    return "(jclass)" +
           trackLocalRefExpr("_mj2x_find_class(env, " + ownerExpr + ", \"" + typeDesc + "\")");
}

inline const char* defaultReturnValue(PrimitiveKind k)
{
    switch (k) {
    case PrimitiveKind::VOID: return "";
    case PrimitiveKind::FLOAT: return "0.0f";
    case PrimitiveKind::DOUBLE: return "0.0";
    case PrimitiveKind::OBJECT: return "NULL";
    default: return "0";
    }
}

inline int loadOpcodeForSort(int sort)
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

inline int returnOpcodeForKind(PrimitiveKind kind)
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

inline PrimitiveKind methodReturnKind(const std::string& methodDesc)
{
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(methodDesc.c_str())).getDescriptor());
}

inline std::string prependReceiverToMethodDesc(const std::string& ownerClassName,
                                               const std::string& methodDesc)
{
    const auto close = methodDesc.find(')');
    if (methodDesc.empty() || methodDesc[0] != '(' || close == std::string::npos) return methodDesc;
    return "(L" + ownerClassName + ";" + methodDesc.substr(1);
}

inline bool isInterfaceDefaultMethod(const CompilerClass& clazz, const jbytecode::tree::MethodNode& method)
{
    if ((clazz.classNode->access & Opcodes::ACC_INTERFACE) == 0) return false;
    if ((method.access & (Opcodes::ACC_ABSTRACT | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE)) != 0)
        return false;
    const auto name = method.name.to_std_string();
    return name != "<init>" && name != "<clinit>";
}

inline std::string utf8RefToString(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

inline std::string typeDescriptorString(const Type& type)
{
    return type.getDescriptor().to_std_string();
}

inline std::string methodTypeDescriptorString(const Type& type)
{
    return type.getSort() == Type::METHOD ? type.getDescriptor().to_std_string() : type.toString();
}

inline const char* boxedClassName(PrimitiveKind kind)
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

inline const char* boxedValueOfDesc(PrimitiveKind kind)
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

inline const char* boxedUnboxMethod(PrimitiveKind kind)
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

inline const char* boxedUnboxDesc(PrimitiveKind kind)
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

/// @brief 异常路径的返回发射（缩进由调用方控制，emitLine 跟随当前层）。
///        异常检查逐点 `return` 对齐 JNIC 直译模型；local-ref 由 JVM 自动回收。
inline void emitExceptionReturn(const std::string& methodDesc, codegen::CSourceStream& out)
{
    if (methodReturnKind(methodDesc) == PrimitiveKind::VOID) {
        out.emitLine("return;");
    }
    else {
        out.appendLineStart("return ")
        .append(defaultReturnValue(methodReturnKind(methodDesc))).append(";");
        out.newLine();
    }
}

// ---- 运算符映射 ----

inline const char* binaryOpToC(BinaryOp op)
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

inline const char* cmpOpToC(CmpOp op)
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

// ---- 异常/控制流辅助 ----

inline std::string exceptionDispatchKey(const std::vector<int>& handlerIndices)
{
    codegen::CSourceStream key;
    bool                   first = true;
    for (int index : handlerIndices) {
        if (!first) key.append(",");
        first = false;
        key.appendInt(index);
    }
    return key.str();
}

inline void removeActiveHandler(std::vector<int>& activeHandlers, int handlerIndex)
{
    for (auto it = activeHandlers.begin(); it != activeHandlers.end(); ++it) {
        if (*it == handlerIndex) {
            activeHandlers.erase(it);
            break;
        }
    }
}

inline bool isProtectedByExceptionHandler(const MIRNode* node)
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

// ============================================================
// Impl（跨文件共享状态与发射器）
// ============================================================

struct StackCBackend::Impl
{
    codegen::CSourceStream header;
    codegen::CSourceStream source;
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
    std::unordered_map<std::string, size_t> stringPoolLengths;
    std::vector<ca::u16>                    stringPoolUnits;
    std::deque<std::string>                 stableUtf8Storage;
    /// pending 异常检查模式：true = lite（只保留 handler 边界检查）。
    bool exceptionCheckLite = false;

    // Cache current method info for codegen
    std::string currentClassName;
    std::string currentMethodName;
    std::string currentMethodDesc;
    std::string currentExceptionDispatchTarget;
    /// 当前发射点的编译期栈深度（块入口来自数据流，块内逐节点累积净效应）。
    /// emitNodeWithExceptionDispatch 静态化栈访问后更新。
    int currentStackDepth = 0;

    /// 当前方法的 label 顺序索引：以 Label* 为 key（进程内身份匹配），值为方法内首次出现顺序。
    /// 每个 compileMethod 开始时重建，用于生成确定性 C label（见 labelIdent）。
    std::unordered_map<jbytecode::core::Label*, unsigned> labelIndex;

    void emitStackInit(int maxStack);
    void emitLocalsInit(const std::string& desc, int access);

    // MIR node → C code
    void   emitNode(const MIRNode* node, codegen::CSourceStream& out);
    void   emitNodeWithExceptionDispatch(const MIRNode* node, codegen::CSourceStream& out,
                                         const std::string& dispatchTarget);
    void   emitConstant(const MIRConstant* node, codegen::CSourceStream& out);    void   emitLocalLoad(const MIRLocalLoad* node, codegen::CSourceStream& out);
    void   emitLocalStore(const MIRLocalStore* node, codegen::CSourceStream& out);
    void   emitIinc(const MIRIinc* node, codegen::CSourceStream& out);
    void   emitBinaryOp(const MIRBinaryOp* node, codegen::CSourceStream& out);
    void   emitUnaryOp(const MIRUnaryOp* node, codegen::CSourceStream& out);
    void   emitCmp(const MIRCmp* node, codegen::CSourceStream& out);
    void   emitCast(const MIRCast* node, codegen::CSourceStream& out);
    void   emitBranch(const MIRBranch* node, codegen::CSourceStream& out);
    void   emitBranchZero(const MIRBranchZero* node, codegen::CSourceStream& out);
    void   emitUncondBranch(const MIRUncondBranch* node, codegen::CSourceStream& out);
    void   emitSwitch(const MIRSwitch* node, codegen::CSourceStream& out);
    void   emitReturn(const MIRReturn* node, codegen::CSourceStream& out);
    void   emitReturnValue(const MIRReturnValue* node, codegen::CSourceStream& out);
    void   emitNewObject(const MIRNewObject* node, codegen::CSourceStream& out);
    void   emitNewArray(const MIRNewArray* node, codegen::CSourceStream& out);
    void   emitMultiNewArray(const MIRMultiNewArray* node, codegen::CSourceStream& out);
    void   emitFieldGet(const MIRFieldGet* node, codegen::CSourceStream& out);
    void   emitFieldPut(const MIRFieldPut* node, codegen::CSourceStream& out);
    void   emitStaticGet(const MIRStaticGet* node, codegen::CSourceStream& out);
    void   emitStaticPut(const MIRStaticPut* node, codegen::CSourceStream& out);
    void   emitInvoke(const MIRInvoke* node, codegen::CSourceStream& out);
    void   emitInvokeDynamic(const MIRInvokeDynamic* node, codegen::CSourceStream& out);
    void   emitUnsupported(const char* what, codegen::CSourceStream& out);
    void   emitCheckCast(const MIRCheckCast* node, codegen::CSourceStream& out);
    void   emitInstanceOf(const MIRInstanceOf* node, codegen::CSourceStream& out);
    void   emitMonitor(const MIRMonitor* node, codegen::CSourceStream& out);
    void   emitThrow(const MIRThrow* node, codegen::CSourceStream& out);
    void   emitArrayLength(const MIRArrayLength* node, codegen::CSourceStream& out);
    void   emitArrayLoad(const MIRArrayLoad* node, codegen::CSourceStream& out);
    void   emitArrayStore(const MIRArrayStore* node, codegen::CSourceStream& out);
    void   emitIntegerDivZeroTrap(const char* rhsAccessor, codegen::CSourceStream& out);
    void   emitNullPointerTrap(const std::string& refExpr, codegen::CSourceStream& out);
    void   emitPendingExceptionCheck(codegen::CSourceStream& out, int indent = 8);
    void   emitFieldIdCache(const std::string& cacheName, const std::string& localName,
                            const std::string& classExpr, const std::string& name,
                            const std::string& desc, bool isStatic, codegen::CSourceStream& out);
    void   emitMethodIdCache(const std::string& cacheName, const std::string& localName,
                             const std::string& classExpr, const std::string& name,
                             const std::string& desc, bool isStatic, codegen::CSourceStream& out);
    void   emitClassRefCache(const std::string& className, codegen::CSourceStream& out);
    size_t stringPoolOffsetFor(const std::string& value);
    size_t stringPoolLengthFor(const std::string& value);
    void   emitStringPool(codegen::CSourceStream& out) const;
    void   emitRegistrationCode(codegen::CSourceStream& out) const;
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
    /// 每块入口栈深度数据流：自建控制流边（converter 不填 successors），
    /// handler 入口深度=1，块内净效应按节点发射文本统计。
    /// 返回 false 表示深度无法收敛（非法字节码，调用方 fail-closed 跳过）。
    bool computeBlockDepths(const std::vector<Block*>& blocks,
                            const std::vector<jbytecode::core::Label*>& handlerLabels,
                            std::vector<int>& entryDepths);
};

// ============================================================
// snippet 渲染（定义在 stackc_snippets.cpp）
// ============================================================

enum class StackCSnippet
{
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

std::string renderStackCSnippet(StackCSnippet key, const codegen::SnippetVars& vars);
void        emitCLine(codegen::CSourceStream& out, int indentLevel, const std::string& line);

// ============================================================
// 注册/代理类生成（定义在 stackc_registration.cpp）
// ============================================================

std::string          hiddenClinitClassNameFor(const std::string& ownerClassName);
std::vector<uint8_t> makeHiddenClinitClassBytes(const std::string& hiddenClassName,
                                                const std::string& hiddenMethodName);
std::vector<uint8_t> makeHiddenInterfaceDefaultClassBytes(const std::string& hiddenClassName,
                                                          const std::string& hiddenMethodName,
                                                          const std::string& hiddenMethodDesc);
void                 clearMethodDebugAndHandlers(MethodNode* method);

}   // namespace mj2x

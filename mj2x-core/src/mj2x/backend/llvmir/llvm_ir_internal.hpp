/// @file llvm_ir_internal.hpp
/// @brief LLVM IR 后端的内部共享声明：类型工具 / JNI 槽表 / 全局字符串表 /
///        互调解析器与发射器接口（实现拆分到 llvm_ir_*.cpp，保持主文件精简）。
#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mj2x/backend/backend.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/jvm_semantics.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/register_ir/reg_instruction.hpp"
#include "mj2x/mir/ssa/ssa_effect.hpp"
#include "mj2x/mir/ssa/ssa_type.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mj2x {
namespace llvmir {

using namespace jbytecode::core;
using namespace jbytecode::tree;

using codegen::mangleMethod;

/// @brief RegId → SSA 类型（槽类型权威表；Compare 结果强制 BOOL）。
using RegisterTypes = std::map<uint32_t, SSAType>;

/// @brief SSAType → LLVM 类型名（REF 为 opaque ptr）。
inline const char* llvmType(SSAType type)
{
    switch (type) {
    case SSAType::BOOL: return "i1";
    case SSAType::I32: return "i32";
    case SSAType::I64: return "i64";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::REF: return "ptr";   // JNI 引用（jobject/jclass/jstring/数组）
    case SSAType::VOID: return "void";
    default: return nullptr;
    }
}

inline bool isSupportedValueType(SSAType type)
{
    return llvmType(type) != nullptr && type != SSAType::VOID;
}

/// @brief JVM 窄类型统一为 i32 计算位宽（boolean/byte/short/char 栈语义）。
inline SSAType widenNarrowType(SSAType type)
{
    return (type == SSAType::BOOL || type == SSAType::I8 || type == SSAType::I16 ||
            type == SSAType::CHAR)
               ? SSAType::I32
               : type;
}

inline const char* llvmTypeWide(SSAType type)
{
    return llvmType(widenNarrowType(type));
}

inline bool hasObservableEffect(const SSAEffect& effect)
{
    return effect.mayThrow || effect.readsMemory || effect.writesMemory ||
           effect.readsExternalState || effect.writesExternalState || effect.callsExternal ||
           effect.resolvesExternal || effect.isBarrier;
}

inline PrimitiveKind returnKind(const std::string& desc)
{
    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，
    // 落点参数按必选口径走函数级局部 arena（结果仅瞬时喂给 jvmDescToPrimitive）。
    ca::str::Utf8StringArena scratch;
    return jvmDescToPrimitive(
        Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str()))
            .getDescriptor(scratch));
}

inline std::vector<PrimitiveKind> argumentKinds(const std::string& desc)
{
    std::vector<PrimitiveKind> kinds;
    auto methodType = Type::getMethodType(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    for (const auto& argument : methodType.getArgumentTypes()) {
        kinds.push_back(jvmSortToPrimitive(argument.getSort()));
    }
    return kinds;
}

inline SSAType primitiveToSsaType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN:
    case PrimitiveKind::BYTE:
    case PrimitiveKind::SHORT:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::INT: return SSAType::I32;   // JVM 窄类型栈位宽 = int
    case PrimitiveKind::LONG: return SSAType::I64;
    case PrimitiveKind::FLOAT: return SSAType::F32;
    case PrimitiveKind::DOUBLE: return SSAType::F64;
    case PrimitiveKind::VOID: return SSAType::VOID;
    case PrimitiveKind::OBJECT: return SSAType::REF;
    default: return SSAType::UNKNOWN;
    }
}

/// @brief JNI 边界类型（native 方法签名；窄类型为 jboolean/jbyte/jchar/jshort）。
inline const char* jniBoundaryType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN:
    case PrimitiveKind::BYTE: return "i8";
    case PrimitiveKind::CHAR:
    case PrimitiveKind::SHORT: return "i16";
    case PrimitiveKind::INT: return "i32";
    case PrimitiveKind::LONG: return "i64";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    case PrimitiveKind::OBJECT: return "ptr";
    default: return nullptr;
    }
}

/// @brief 字段描述符 → JNI 访问种类（field_load_*/field_store_* helper 后缀）。
inline const char* fieldKindFor(const std::string& desc)
{
    if (desc.empty()) return nullptr;
    switch (desc[0]) {
    case 'Z': return "bool";
    case 'B': return "byte";
    case 'C': return "char";
    case 'S': return "short";
    case 'I': return "int";
    case 'J': return "long";
    case 'F': return "float";
    case 'D': return "double";
    case 'L':
    case '[': return "ref";
    default: return nullptr;
    }
}

/// @brief 字段 helper 的返回值 LLVM 类型（窄字段在 helper 内提升到 i32）。
inline const char* fieldHelperReturnType(const char* kind)
{
    if (kind == nullptr) return nullptr;
    const std::string k(kind);
    if (k == "long") return "i64";
    if (k == "float") return "float";
    if (k == "double") return "double";
    if (k == "ref") return "ptr";
    return "i32";   // bool/byte/char/short/int
}

/// @brief 数组元素 SSAType → helper 后缀（BALOAD 组件类型为 BYTE；无 BOOL 形态）。
inline const char* arrayKindFor(SSAType elementType)
{
    switch (elementType) {
    case SSAType::BOOL:
    case SSAType::I8: return "byte";   // BALOAD/BASTORE 组件类型固定 BYTE
    case SSAType::I16: return "short";
    case SSAType::CHAR: return "char";
    case SSAType::I32: return "int";
    case SSAType::I64: return "long";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::REF: return "ref";
    default: return nullptr;
    }
}

/// @brief 值 SSAType → 装箱/拆箱 helper 后缀（indy 参数/结果；REF 不装箱）。
inline const char* boxKindFor(SSAType type)
{
    switch (type) {
    case SSAType::BOOL: return "boolean";
    case SSAType::I8: return "byte";
    case SSAType::I16: return "short";
    case SSAType::CHAR: return "char";
    case SSAType::I32: return "int";
    case SSAType::I64: return "long";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    default: return nullptr;
    }
}

/// @brief 拆箱 helper 返回类型（JVM 窄类型统一为 i32 计算位宽，与槽类型一致）。
inline const char* unboxReturnType(SSAType type)
{
    switch (widenNarrowType(type)) {
    case SSAType::I32: return "i32";
    case SSAType::I64: return "i64";
    case SSAType::F32: return "float";
    case SSAType::F64: return "double";
    case SSAType::REF: return "ptr";
    default: return nullptr;
    }
}

/// @brief 方法返回类型 → JNI 调用 helper 后缀（CallXxxMethodA 分派）。
inline const char* callKindFor(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "bool";
    case PrimitiveKind::BYTE: return "byte";
    case PrimitiveKind::CHAR: return "char";
    case PrimitiveKind::SHORT: return "short";
    case PrimitiveKind::INT: return "int";
    case PrimitiveKind::LONG: return "long";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::OBJECT: return "object";
    case PrimitiveKind::VOID: return "void";
    default: return nullptr;
    }
}

/// @brief JNI 函数表槽位（jni.h `JNINativeInterface_` 成员序号，含 reserved0-3）。
namespace jni_slot {

constexpr int64_t FindClass            = 6;
constexpr int64_t ThrowNew             = 14;
constexpr int64_t AllocObject          = 27;
constexpr int64_t GetObjectClass       = 31;
constexpr int64_t IsInstanceOf         = 32;
constexpr int64_t GetMethodID          = 33;
constexpr int64_t CallObjectMethodA    = 36;
constexpr int64_t CallBooleanMethodA   = 39;
constexpr int64_t CallByteMethodA      = 42;
constexpr int64_t CallCharMethodA      = 45;
constexpr int64_t CallShortMethodA     = 48;
constexpr int64_t CallIntMethodA       = 51;
constexpr int64_t CallLongMethodA      = 54;
constexpr int64_t CallFloatMethodA     = 57;
constexpr int64_t CallDoubleMethodA    = 60;
constexpr int64_t CallVoidMethodA      = 63;
constexpr int64_t CallNonvirtualObjectMethodA  = 66;
constexpr int64_t CallNonvirtualBooleanMethodA = 69;
constexpr int64_t CallNonvirtualByteMethodA    = 72;
constexpr int64_t CallNonvirtualCharMethodA    = 75;
constexpr int64_t CallNonvirtualShortMethodA   = 78;
constexpr int64_t CallNonvirtualIntMethodA     = 81;
constexpr int64_t CallNonvirtualLongMethodA    = 84;
constexpr int64_t CallNonvirtualFloatMethodA   = 87;
constexpr int64_t CallNonvirtualDoubleMethodA  = 90;
constexpr int64_t CallNonvirtualVoidMethodA    = 93;
constexpr int64_t GetFieldID            = 94;
constexpr int64_t GetObjectField        = 95;
constexpr int64_t GetBooleanField       = 96;
constexpr int64_t GetByteField          = 97;
constexpr int64_t GetCharField          = 98;
constexpr int64_t GetShortField         = 99;
constexpr int64_t GetIntField           = 100;
constexpr int64_t GetLongField          = 101;
constexpr int64_t GetFloatField         = 102;
constexpr int64_t GetDoubleField        = 103;
constexpr int64_t SetObjectField        = 104;
constexpr int64_t SetBooleanField       = 105;
constexpr int64_t SetByteField          = 106;
constexpr int64_t SetCharField          = 107;
constexpr int64_t SetShortField         = 108;
constexpr int64_t SetIntField           = 109;
constexpr int64_t SetLongField          = 110;
constexpr int64_t SetFloatField         = 111;
constexpr int64_t SetDoubleField        = 112;
constexpr int64_t GetStaticMethodID     = 113;
constexpr int64_t CallStaticObjectMethodA  = 116;
constexpr int64_t CallStaticBooleanMethodA = 119;
constexpr int64_t CallStaticByteMethodA    = 122;
constexpr int64_t CallStaticCharMethodA    = 125;
constexpr int64_t CallStaticShortMethodA   = 128;
constexpr int64_t CallStaticIntMethodA     = 131;
constexpr int64_t CallStaticLongMethodA    = 134;
constexpr int64_t CallStaticFloatMethodA   = 137;
constexpr int64_t CallStaticDoubleMethodA  = 140;
constexpr int64_t CallStaticVoidMethodA    = 143;
constexpr int64_t GetStaticFieldID     = 144;
constexpr int64_t GetStaticObjectField = 145;
constexpr int64_t GetStaticBooleanField = 146;
constexpr int64_t GetStaticByteField   = 147;
constexpr int64_t GetStaticCharField   = 148;
constexpr int64_t GetStaticShortField  = 149;
constexpr int64_t GetStaticIntField    = 150;
constexpr int64_t GetStaticLongField   = 151;
constexpr int64_t GetStaticFloatField  = 152;
constexpr int64_t GetStaticDoubleField = 153;
constexpr int64_t SetStaticObjectField = 154;
constexpr int64_t SetStaticBooleanField = 155;
constexpr int64_t SetStaticByteField   = 156;
constexpr int64_t SetStaticCharField   = 157;
constexpr int64_t SetStaticShortField  = 158;
constexpr int64_t SetStaticIntField    = 159;
constexpr int64_t SetStaticLongField   = 160;
constexpr int64_t SetStaticFloatField  = 161;
constexpr int64_t SetStaticDoubleField = 162;
constexpr int64_t NewStringUTF         = 167;
constexpr int64_t IsSameObject         = 24;
constexpr int64_t NewGlobalRef         = 21;
constexpr int64_t DeleteGlobalRef      = 22;
constexpr int64_t NewLocalRef          = 25;
constexpr int64_t GetArrayLength       = 171;
constexpr int64_t NewObjectArray       = 172;
constexpr int64_t GetObjectArrayElement = 173;
constexpr int64_t SetObjectArrayElement = 174;
constexpr int64_t NewBooleanArray      = 175;
constexpr int64_t NewByteArray         = 176;
constexpr int64_t NewCharArray         = 177;
constexpr int64_t NewShortArray        = 178;
constexpr int64_t NewIntArray          = 179;
constexpr int64_t NewLongArray         = 180;
constexpr int64_t NewFloatArray        = 181;
constexpr int64_t NewDoubleArray       = 182;
constexpr int64_t GetBooleanArrayRegion = 199;
constexpr int64_t GetByteArrayRegion   = 200;
constexpr int64_t GetCharArrayRegion   = 201;
constexpr int64_t GetShortArrayRegion  = 202;
constexpr int64_t GetIntArrayRegion    = 203;
constexpr int64_t GetLongArrayRegion   = 204;
constexpr int64_t GetFloatArrayRegion  = 205;
constexpr int64_t GetDoubleArrayRegion = 206;
constexpr int64_t SetBooleanArrayRegion = 207;
constexpr int64_t SetByteArrayRegion   = 208;
constexpr int64_t SetCharArrayRegion   = 209;
constexpr int64_t SetShortArrayRegion  = 210;
constexpr int64_t SetIntArrayRegion    = 211;
constexpr int64_t SetLongArrayRegion   = 212;
constexpr int64_t SetFloatArrayRegion  = 213;
constexpr int64_t SetDoubleArrayRegion = 214;
constexpr int64_t MonitorEnter         = 217;
constexpr int64_t MonitorExit          = 218;
constexpr int64_t Throw                = 13;
constexpr int64_t ExceptionOccurred    = 15;
constexpr int64_t ExceptionClear       = 17;
constexpr int64_t ExceptionCheck       = 228;
}   // namespace jni_slot

/// @brief Resolved intra-class static call target for the LLVM IR slice.
struct CallTarget
{
    std::string          symbol;   ///< internal helper symbol（@mj2x_llvm_Java_...）
    SSAType              returnType = SSAType::UNKNOWN;
    std::vector<SSAType> argTypes;
};

/// @brief 解析并预检 LLVM IR 切片内的静态互调目标（实现见 llvm_ir_backend.cpp）。
/// @details 目标必须满足：INVOKESTATIC、目标类在编译索引内（非 library）、
/// 签名纯标量、方法体可转换（递归预检，含嵌套调用）；结果按 MethodID 缓存。
class InternalCallResolver
{
public:
    InternalCallResolver(CompilerIndex& index, const std::string& className)
        : index_(index)
        , className_(className)
    {}

    bool resolve(const SSAMethodRef& method, CallTarget& target, std::string& reason);

private:
    CompilerMethod* findTarget(const MethodID& id, std::string& reason);
    bool            fillTargetFromDescriptor(const std::string& owner, MethodNode& node,
                                             CallTarget& target, std::string& reason);
    bool            analyze(const std::string& owner, MethodNode& node, CallTarget& target,
                            std::string& reason);

    CompilerIndex&                     index_;
    const std::string&                 className_;
    std::unordered_map<MethodID, CallTarget> cache_;
    std::unordered_set<MethodID>       visiting_;
};

/// @brief 延迟边拷贝（最终 br 目标前的 store 序列）。
struct PendingEdge
{
    std::string           label;
    const RegSuccessor*   target = nullptr;
};

/// @brief 共享全局字符串表：发射端 intern 得到 @_mj2x_llvm_str_<N>，
/// finalize 时统一输出常量定义。
class GlobalStringTable
{
public:
    std::string intern(const std::string& text)
    {
        for (size_t index = 0; index < strings_.size(); ++index) {
            if (strings_[index] == text) return "@_mj2x_llvm_str_" + std::to_string(index);
        }
        const size_t index = strings_.size();
        strings_.push_back(text);
        return "@_mj2x_llvm_str_" + std::to_string(index);
    }

    std::string emitDefinitions() const
    {
        std::ostringstream out;
        for (size_t index = 0; index < strings_.size(); ++index) {
            out << "@_mj2x_llvm_str_" << index << " = private unnamed_addr constant ["
                << (strings_[index].size() + 1) << " x i8] c\"";
            for (const unsigned char ch : strings_[index]) {
                if (ch == '\\' || ch == '"') {
                    out << '\\' << static_cast<char>(ch);
                }
                else if (ch < 32 || ch > 126) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%02X", ch);
                    out << buf;
                }
                else {
                    out << static_cast<char>(ch);
                }
            }
            out << "\\00\"\n";
        }
        return out.str();
    }

private:
    std::vector<std::string> strings_;
};

// ---- 验证（llvm_ir_validation.cpp） ----

bool recordValue(RegisterTypes& types, RegValue value, std::string& reason);
bool recordOperand(RegisterTypes& types, const RegOperand& operand, std::string& reason);
bool recordCopies(RegisterTypes& types, const std::vector<RegCopy>& copies, std::string& reason);
bool recordSuccessor(RegisterTypes& types, const RegSuccessor& successor, std::string& reason);
bool isSupportedBinary(const RegInstruction& instruction, std::string& reason);
bool recordInstruction(RegisterTypes& types, const RegInstruction& instruction,
                       InternalCallResolver* resolver, size_t indySiteCount, std::string& reason);
bool collectRegisterTypes(const RegFunction& function, RegisterTypes& types,
                          InternalCallResolver* resolver, std::string& reason);

// ---- 发射器（llvm_ir_emitter.cpp） ----

class LlvmFunctionEmitter;

/// @brief 发射一个 RegIR 函数为 textual LLVM IR（类定义在 emitter.cpp）。
/// @param indySiteBase 文件级 invokedynamic 站点表偏移（本方法首个站点的全局
///                     索引；方法内局部索引 = 全局索引 - indySiteBase）。
/// @param runtimeProtection 非空时在方法入口注入反侦测检查点（mj2x-rt
///                          rt_protect，编译侧链接 mj2x_rt_protect.c）。
/// @param checkpointGlobals 出参：本函数注入的检查点状态全局（模块级输出用）。
/// @param runtimeDeclares 出参：本函数用到的 JDK intrinsic 直调声明。
bool emitLlvmFunction(const RegFunction& function, const RegisterTypes& registerTypes,
                      const std::string& functionName, SSAType returnType,
                      InternalCallResolver* resolver, GlobalStringTable& strings,
                      size_t indySiteBase,
                      const codegen::RuntimeProtectionOptions* runtimeProtection,
                      uint32_t* checkpointCounter, std::vector<std::string>* checkpointGlobals,
                      std::vector<std::string>* runtimeDeclares, std::string& source,
                      std::string& reason);

// ---- 共享 helper 发射（llvm_ir_helpers.cpp） ----

std::string emitThrowArithmeticHelper();
std::string emitObjectHelpers();

/// @brief invokedynamic 运行时解析 helper 集合（box/unbox、MethodType、Class 对象、
/// MethodHandle 解析、JNI mid 直调），仅含 indy 站点时生成。
std::string emitIndyHelpers();

/// @brief 单个 invokedynamic 站点的 bootstrap 函数（对齐 RegC emitIndyBootstrapFunction：
/// Lookup + privateLookupIn + MethodType + bsm 调用 + CallSite.getTarget + 全局缓存）。
std::string emitIndyBootstrapFunction(size_t index, const RegFunction::InvokeDynamicSite& site);

/// @brief 反侦测检查点模块级声明（%mj2x_rt_checkpoint + extern 调用；链接侧由
///        编译管线附加 mj2x-rt rt_protect 源）。
std::string emitRuntimeProtectionDeclarations();

}   // namespace llvmir
}   // namespace mj2x

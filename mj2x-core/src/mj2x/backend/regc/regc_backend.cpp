#include "regc_backend.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/mir/passes/reg_transform_passes.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// JNI mangling is shared with StackC via codegen::; alias the symbols so
// existing call sites in this file stay one-line.
using codegen::CSourceStream;
using codegen::mangleMethod;
using codegen::mangleName;

const char* jniType(PrimitiveKind kind)
{
    switch (kind) {
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

const char* regcType(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "bool";
    case PrimitiveKind::BYTE: return "int8_t";
    case PrimitiveKind::CHAR: return "uint16_t";
    case PrimitiveKind::SHORT: return "int16_t";
    case PrimitiveKind::INT: return "int32_t";
    case PrimitiveKind::LONG: return "int64_t";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    default: return "mj2x_ref";
    }
}

const char* defaultReturnValue(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::VOID: return "";
    case PrimitiveKind::FLOAT: return "0.0f";
    case PrimitiveKind::DOUBLE: return "0.0";
    case PrimitiveKind::OBJECT: return "NULL";
    default: return "0";
    }
}

PrimitiveKind returnKind(const std::string& desc)
{
    return jvmDescToPrimitive(Type::getReturnType(ca::str::Utf8StringRef::from_cstr(desc.c_str()))
                                  .getDescriptor()
                                  .to_std_string());
}

std::vector<PrimitiveKind> argumentKinds(const std::string& desc)
{
    std::vector<PrimitiveKind> kinds;
    auto methodType = Type::getMethodType(ca::str::Utf8StringRef::from_cstr(desc.c_str()));
    for (const auto& argType : methodType.getArgumentTypes()) {
        kinds.push_back(jvmSortToPrimitive(argType.getSort()));
    }
    return kinds;
}

bool isCompatibilityRiskCall(const std::string& owner, const std::string& name)
{
    return owner == "java/lang/Class" || owner == "java/lang/ClassLoader" ||
           owner == "java/lang/Thread" || owner == "java/io/InputStream" ||
           owner == "java/io/ByteArrayInputStream" ||
           (owner == "java/lang/Object" && name == "getClass") ||
           owner.rfind("java/lang/reflect/", 0) == 0 ||
           owner.rfind("java/util/concurrent/", 0) == 0 ||
           (owner == "java/lang/System" &&
            (name == "setSecurityManager" || name == "getSecurityManager" || name == "exit"));
}

bool shouldKeepBytecodeForCompatibility(const MethodNode& method, const std::vector<Block*>& blocks,
                                        std::string& reason)
{
    if (method.name.to_std_string().rfind("lambda$", 0) == 0) {
        reason = "lambda body remains on bytecode path for invokedynamic compatibility";
        return true;
    }
    if (method.tryCatchBlocks.size() > 4) {
        reason = "large exception-table method remains on bytecode path";
        return true;
    }
    for (auto* block : blocks) {
        for (const auto& node : block->nodes) {
            if (node->kind == MIRKind::INVOKE_DYNAMIC) {
                reason = "invokedynamic remains on bytecode path for compatibility";
                return true;
            }
            if (node->kind == MIRKind::INVOKE) {
                const auto* invoke = static_cast<const MIRInvoke*>(node.get());
                if (isCompatibilityRiskCall(invoke->owner, invoke->name)) {
                    reason = "high-risk runtime call remains on bytecode path: " + invoke->owner +
                             "." + invoke->name;
                    return true;
                }
            }
        }
    }
    return false;
}

std::string emitFailureBody(const std::string& reason, PrimitiveKind retKind)
{
    std::ostringstream out;
    out << "    /* RegC lowering failed: " << reason << " */\n";
    if (retKind == PrimitiveKind::VOID) {
        out << "    return;\n";
    }
    else {
        out << "    return " << defaultReturnValue(retKind) << ";\n";
    }
    return out.str();
}

std::string emitJniWrapper(const std::string& className, const MethodNode& method,
                           const RegCEmitResult& emitted)
{
    const std::string methodName = method.name.to_std_string();
    const std::string methodDesc = method.desc.to_std_string();
    const bool        isStatic   = (method.access & Opcodes::ACC_STATIC) != 0;
    const auto        retKind    = returnKind(methodDesc);
    const auto        args       = argumentKinds(methodDesc);

    std::ostringstream out;
    out << "JNIEXPORT " << jniType(retKind) << " JNICALL\n";
    out << mangleMethod(className, methodName, methodDesc) << "(JNIEnv* env, "
        << (isStatic ? "jclass clazz" : "jobject thisObject");
    for (size_t i = 0; i < args.size(); ++i) {
        out << ", " << jniType(args[i]) << " p" << i;
    }
    out << ") {\n";
    out << "    (void)" << (isStatic ? "clazz" : "thisObject") << ";\n";
    out << "    _mj2x_regc_env = env;\n";
    out << "    mj2x_regc_init_context(&_mj2x_regc_ctx, env, _mj2x_regc_throw_runtime);\n";

    if (!emitted.ok) {
        out << emitFailureBody(emitted.reason, retKind);
        out << "}\n\n";
        return out.str();
    }

    out << "    ";
    if (retKind != PrimitiveKind::VOID) out << "return (" << jniType(retKind) << ")";
    out << emitted.functionName << "(";
    bool first     = true;
    auto appendArg = [&](const std::string& expr, const char* type) {
        if (!first) out << ", ";
        first = false;
        out << "(" << type << ")(" << expr << ")";
    };
    if (!isStatic) appendArg("thisObject", "mj2x_ref");
    for (size_t i = 0; i < args.size(); ++i) {
        appendArg("p" + std::to_string(i), regcType(args[i]));
    }
    out << ");\n";
    if (retKind == PrimitiveKind::VOID) out << "    return;\n";
    out << "}\n\n";
    return out.str();
}


MethodNode* findMethod(CompilerClass& clazz, const char* name, const char* desc)
{
    for (auto* method : clazz.classNode->methods) {
        if (method->name.to_std_string() == name && method->desc.to_std_string() == desc) {
            return method;
        }
    }
    return nullptr;
}

}   // namespace

struct RegCBackend::Impl
{
    std::ostringstream source;
    RegCCachePool      cachePool;
};

RegCBackend::RegCBackend()
    : impl_(std::make_unique<Impl>())
{}

RegCBackend::~RegCBackend() = default;

BackendResult RegCBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                         const std::vector<Block*>& blocks)
{
    BackendResult     backendResult;
    auto*             mn         = method.methodNode;
    const std::string methodName = mn->name.to_std_string();
    const std::string methodDesc = mn->desc.to_std_string();
    const bool        isStatic   = (mn->access & Opcodes::ACC_STATIC) != 0;

    std::string reason;
    if (className.find('$') != std::string::npos) {
        reason = "inner class method remains on bytecode path for compatibility";
        printf("[RegCBackend] Skipping %s.%s%s: %s\n",
               className.c_str(),
               methodName.c_str(),
               methodDesc.c_str(),
               reason.c_str());
        return backendResult;
    }
    if (shouldKeepBytecodeForCompatibility(*mn, blocks, reason)) {
        printf("[RegCBackend] Skipping %s.%s%s: %s\n",
               className.c_str(),
               methodName.c_str(),
               methodDesc.c_str(),
               reason.c_str());
        return backendResult;
    }

    SSABuilder ssaBuilder;
    // RegCBackend is a facade over the existing IR pipeline: L1 blocks are
    // supplied by Pipeline, then this backend owns the SSA and RegIR handoff.
    auto ssa = ssaBuilder.build(
        blocks, className, methodName, methodDesc, isStatic, mn->maxLocals, reason);
    if (!ssa) {
        backendResult.text = emitJniWrapper(className, *mn, RegCEmitResult{false, {}, {}, reason});
        return backendResult;
    }

    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) {
        backendResult.text = emitJniWrapper(className, *mn, RegCEmitResult{false, {}, {}, reason});
        return backendResult;
    }
    eliminateDeadRegInstructions(*reg);
    applyRegTransformPasses(*reg);

    RegCEmitter emitter(&impl_->cachePool);
    auto        emitted = emitter.emitFunction(*reg);
    if (!emitted.ok) {
        backendResult.text = emitJniWrapper(className, *mn, emitted);
        return backendResult;
    }

    // Keep the stackless helper as the trace surface and add a thin JNI entry
    // point only for JVM native binding.  The wrapper must not reintroduce a
    // StackC-style operand stack.
    std::ostringstream methodSource;
    methodSource << emitted.source << "\n";
    methodSource << emitJniWrapper(className, *mn, emitted);

    backendResult.kind = BackendResult::Kind::C_SOURCE;
    backendResult.text = methodSource.str();
    impl_->source << methodSource.str();
    return backendResult;
}

void RegCBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                  const BackendResult& result)
{
    // Failed lowering leaves the Java bytecode intact.  This prevents a class
    // from being rewritten to native when no working RegC body was emitted.
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    auto* mn = method.methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack   = 0;
    mn->maxLocals  = 0;
    clazz.modified = true;
    // Native-loader clinit 注入由 NativeLoaderPostprocessor 统一负责（见
    // postprocessor/native_loader_call.hpp），不再在 backend 内重复。
}

void RegCBackend::finalize(const CompilerIndex& index)
{
    (void)index;

    // strtol + errno back the Integer.parseInt intrinsic body emitted below.
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdbool.h");
    includes.addSystem("limits.h");
    includes.addSystem("stdint.h");
    includes.addSystem("stddef.h");
    includes.addSystem("stdarg.h");
    includes.addSystem("math.h");
    includes.addSystem("stdio.h");
    includes.addSystem("stdlib.h");
    includes.addSystem("string.h");
    includes.addSystem("errno.h");
    includes.addProject("regc/mj2x_regc_crt.h");

    std::ostringstream full;
    full << "/* Generated by mj2x RegCBackend */\n";
    full << includes.render();
    full << "\n";
    const std::string stringClassCacheName = impl_->cachePool.classCache("java/lang/String");
    const std::string objectToStringMethodCacheName = impl_->cachePool.instanceMethodCache(
        "java/lang/Object", "toString", "()Ljava/lang/String;");
    const std::string stringValueOfFloatMethodCacheName =
        impl_->cachePool.staticMethodCache("java/lang/String", "valueOf", "(F)Ljava/lang/String;");
    const std::string stringValueOfDoubleMethodCacheName =
        impl_->cachePool.staticMethodCache("java/lang/String", "valueOf", "(D)Ljava/lang/String;");
    const std::string floatParseFloatMethodCacheName = impl_->cachePool.staticMethodCache(
        "java/lang/Float", "parseFloat", "(Ljava/lang/String;)F");
    const std::string doubleParseDoubleMethodCacheName = impl_->cachePool.staticMethodCache(
        "java/lang/Double", "parseDouble", "(Ljava/lang/String;)D");
    const std::string nullPointerExceptionClassCacheName =
        impl_->cachePool.classCache("java/lang/NullPointerException");
    const std::string classCastExceptionClassCacheName =
        impl_->cachePool.classCache("java/lang/ClassCastException");
    const std::string outOfMemoryErrorClassCacheName =
        impl_->cachePool.classCache("java/lang/OutOfMemoryError");
    const std::string stringIndexOutOfBoundsClassCacheName =
        impl_->cachePool.classCache("java/lang/StringIndexOutOfBoundsException");
    const std::string unsupportedOperationExceptionClassCacheName =
        impl_->cachePool.classCache("java/lang/UnsupportedOperationException");
    const std::string cacheDeclarations = impl_->cachePool.renderDeclarations();
    const std::string emittedSource     = impl_->source.str();
    emitRuntimeHelpers(full,
                       cacheDeclarations,
                       stringClassCacheName,
                       objectToStringMethodCacheName,
                       stringValueOfFloatMethodCacheName,
                       stringValueOfDoubleMethodCacheName,
                       floatParseFloatMethodCacheName,
                       doubleParseDoubleMethodCacheName,
                       nullPointerExceptionClassCacheName,
                       classCastExceptionClassCacheName,
                       outOfMemoryErrorClassCacheName,
                       stringIndexOutOfBoundsClassCacheName,
                       unsupportedOperationExceptionClassCacheName);
    emitRegCHelperPrelude(full);
    full << emittedSource;

    sourceCode_ = full.str();
    if (!cOutputPath_.empty()) {
        std::ofstream out(cOutputPath_, std::ios::binary);
        if (out) {
            out << sourceCode_;
            printf("[RegCBackend] Written: %s (%zu bytes)\n",
                   cOutputPath_.c_str(),
                   sourceCode_.size());
        }
        else {
            printf("[RegCBackend] ERROR: cannot write %s\n", cOutputPath_.c_str());
        }
    }
}

}   // namespace mj2x

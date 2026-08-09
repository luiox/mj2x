#include "stackc_backend.hpp"
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"
#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"
#include "mj2x/codegen/code_snippet.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include <fstream>
#include <string>

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::nativeFunctionName;

namespace {

/// UTF-8 -> UTF-16 code units（Java 字符串常量来自 class 文件，描述符/标识符均为 ASCII）。
/// 非法字节序列按替换字符处理，避免生成器侧崩溃。
std::vector<ca::u16> utf8ToUtf16Units(const std::string& value)
{
    std::vector<ca::u16> units;
    units.reserve(value.size());
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        ca::u32 codePoint = 0;
        int extra = 0;
        if (c < 0x80) {
            codePoint = c;
        }
        else if ((c & 0xE0) == 0xC0) {
            codePoint = c & 0x1F;
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            codePoint = c & 0x0F;
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            codePoint = c & 0x07;
            extra = 3;
        }
        else {
            units.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(extra) + 1 > value.size()) {
            units.push_back(0xFFFD);
            break;
        }
        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(value[i + k]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(value[i + k]) & 0x3F);
        }
        if (!valid) {
            units.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (codePoint <= 0xFFFF) {
            units.push_back(static_cast<ca::u16>(codePoint));
        }
        else {
            const ca::u32 v = codePoint - 0x10000;
            units.push_back(static_cast<ca::u16>(0xD800 + (v >> 10)));
            units.push_back(static_cast<ca::u16>(0xDC00 + (v & 0x3FF)));
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return units;
}

}   // namespace

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

    CSourceStream funcOut;

    // JNI function signature
    bool isStatic = (mn->access & Opcodes::ACC_STATIC) != 0;
    auto retType =
        jvmDescToPrimitive(Type::getReturnType(mn->desc).getDescriptor());

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

    funcOut.appendLineStart("static ").append(primitiveToJNIType(retType)).append(" JNICALL");
    funcOut.newLine();
    funcOut.append(funcName).append("(JNIEnv* env, ")
        .append(isStatic ? "jclass clazz" : "jobject this");

    // Add method parameters
    auto desc     = Type::getMethodType(mn->desc);
    auto argTypes = desc.getArgumentTypes();
    int  slot     = 0;
    int  paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut.append(", ").append(primitiveToJNIType(pk)).append(" p").appendInt(paramIdx);
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut.append(") {");
    funcOut.newLine();
    funcOut.indent();

    // Stack and locals declaration
    int maxLocalsVal = mn->maxLocals > 0 ? mn->maxLocals : 1;
    funcOut.appendLineStart("union jvalue stack[")
        .appendInt(mn->maxStack > 0 ? mn->maxStack : 1).append("];");
    funcOut.newLine();
    funcOut.appendLineStart("union jvalue locals[").appendInt(maxLocalsVal).append("];");
    funcOut.newLine();
    funcOut.emitLine("int sp = -1;");
    funcOut.emitLine("jobject _mj2x_refs[_MJ2X_LOCAL_REF_LIMIT];");
    funcOut.emitLine("int _mj2x_ref_count = 0;");
    if (isStatic) {
        funcOut.emitLine("jclass _mj2x_owner_class = clazz;");
    }
    else {
        funcOut.appendLineStart("jclass _mj2x_owner_class = (jclass)")
            .append(trackLocalRefExpr("(*env)->GetObjectClass(env, this)")).append(";");
        funcOut.newLine();
    }
    funcOut.emitBlankLine();

    // Param init
    slot = 0;
    if (!isStatic) {
        funcOut.appendLineStart("locals[").appendInt(slot).append("].l = this;");
        funcOut.newLine();
        slot++;
    }
    paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut.appendLineStart("locals[").appendInt(slot).append("]")
            .append(stackValueAccessor(pk)).append(" = (")
            .append(narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk)).append(")p")
            .appendInt(paramIdx).append(";");
        funcOut.newLine();
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut.emitBlankLine();

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

    // Build deterministic per-method label index. Walk blocks in order; assign a
    // sequential index to every label the first time it is referenced, whether as a
    // LABEL definition or as a jump/switch target. The Label* is used only as a map
    // key for identity within this process — it never reaches the output, so two
    // independent CLI processes produce identical C labels (L0, L1, ...).
    impl_->labelIndex.clear();
    auto assignLabelIndex = [&](jbytecode::core::Label* lbl) {
        if (lbl == nullptr) return;
        impl_->labelIndex.emplace(lbl, static_cast<unsigned>(impl_->labelIndex.size()));
    };
    for (auto* block : blocks) {
        for (auto& node : block->nodes) {
            switch (node->kind) {
            case MIRKind::LABEL:
                assignLabelIndex(static_cast<const MIRLabel*>(node.get())->labelNode);
                break;
            case MIRKind::BRANCH: assignLabelIndex(static_cast<const MIRBranch*>(node.get())->targetLabel); break;
            case MIRKind::BRANCH_ZERO:
                assignLabelIndex(static_cast<const MIRBranchZero*>(node.get())->targetLabel);
                break;
            case MIRKind::UNCOND_BRANCH:
                assignLabelIndex(static_cast<const MIRUncondBranch*>(node.get())->targetLabel);
                break;
            case MIRKind::SWITCH: {
                const auto* sw = static_cast<const MIRSwitch*>(node.get());
                for (const auto& c : sw->cases) assignLabelIndex(c.targetLabel);
                assignLabelIndex(sw->defaultLabel);
                break;
            }
            default: break;
            }
        }
    }
    for (const auto& h : exceptionHandlers) {
        assignLabelIndex(h.handler);
    }

    // Emit blocks
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        auto* block = blocks[bi];
        funcOut.appendLineStart("/* B").appendInt(static_cast<int64_t>(bi)).append(" */");
        funcOut.newLine();

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
        funcOut.emitBlankLine();
    }

    for (const auto& dispatch : exceptionDispatches) {
        funcOut.append(dispatch.label).append(":");
        funcOut.newLine();
        funcOut.emitLine("{");
        funcOut.indent();
        funcOut.appendLineStart("jthrowable _mj2x_ex = (jthrowable)")
            .append(trackLocalRefExpr("(*env)->ExceptionOccurred(env)")).append(";");
        funcOut.newLine();
        funcOut.emitLine("(*env)->ExceptionClear(env);");
        funcOut.emitLine("if (_mj2x_ex == NULL) {");
        funcOut.indent();
        emitExceptionReturn(impl_->currentMethodDesc, funcOut);
        funcOut.dedent();
        funcOut.emitLine("}");
        for (int handlerIndex : dispatch.handlerIndices) {
            const auto& handler = exceptionHandlers[static_cast<size_t>(handlerIndex)];
            if (handler.catchType.empty()) {
                funcOut.emitLine("sp = -1;");
                funcOut.emitLine("stack[++sp].l = _mj2x_ex;");
                funcOut.appendLineStart("goto ")
                    .append(labelIdent(impl_->labelIndex.at(handler.handler))).append(";");
                funcOut.newLine();
            }
            else {
                funcOut.appendLineStart("if ((*env)->IsInstanceOf(env, _mj2x_ex, ")
                    .append(classExprForDescriptor(handler.catchType)).append(")) {");
                funcOut.newLine();
                funcOut.indent();
                funcOut.emitLine("sp = -1;");
                funcOut.emitLine("stack[++sp].l = _mj2x_ex;");
                funcOut.appendLineStart("goto ")
                    .append(labelIdent(impl_->labelIndex.at(handler.handler))).append(";");
                funcOut.newLine();
                funcOut.dedent();
                funcOut.emitLine("}");
            }
        }
        funcOut.emitLine("(*env)->Throw(env, _mj2x_ex);");
        emitExceptionReturn(impl_->currentMethodDesc, funcOut);
        funcOut.dedent();
        funcOut.emitLine("}");
        funcOut.emitBlankLine();
    }

    funcOut.emitLine("}");
    funcOut.emitBlankLine();
    impl_->source.append(funcOut.str());

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

// ============================================================
// finalize：组装源文件
// ============================================================

void StackCBackend::finalize(const CompilerIndex& index)
{
    // Build final source: header + all compiled function code
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdint.h");   // uint32_t/uint64_t（整数环绕运算）
    includes.addSystem("math.h");     // NAN/INFINITY（浮点常量）

    CSourceStream full;
    full.emitLine("/* Generated by mj2x StackCBackend */");
    full.append(includes.render());
    full.emitBlankLine();
    full.append(backend::stackCRuntimePrelude());
    impl_->emitStringPool(full);
    full.append(impl_->source.str());
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

// ============================================================
// 字符串池（UTF-16 code units，防 strings 扫描）
// ============================================================

size_t StackCBackend::Impl::stringPoolOffsetFor(const std::string& value)
{
    auto found = stringPoolOffsets.find(value);
    if (found != stringPoolOffsets.end()) return found->second;

    const auto units   = utf8ToUtf16Units(value);
    const auto offset  = stringPoolUnits.size();
    stringPoolOffsets.emplace(value, offset);
    stringPoolLengths.emplace(value, units.size());
    stringPoolUnits.insert(stringPoolUnits.end(), units.begin(), units.end());
    stringPoolUnits.push_back(0);
    return offset;
}

size_t StackCBackend::Impl::stringPoolLengthFor(const std::string& value)
{
    return stringPoolLengths.at(value);
}

void StackCBackend::Impl::emitStringPool(codegen::CSourceStream& out) const
{
    if (stringPoolUnits.empty()) return;

    out.appendLineStart("static const uint16_t _mj2x_string_pool[] = {");
    for (size_t i = 0; i < stringPoolUnits.size(); ++i) {
        if (i % 16 == 0) out.append("\n    ");
        out.appendInt(static_cast<int64_t>(stringPoolUnits[i]));
        if (i + 1 < stringPoolUnits.size()) out.append(", ");
    }
    out.append("\n};");
    out.newLine();
    out.emitBlankLine();
}

// ============================================================
// MIR Node → C code（dispatch；各发射器分散在 stackc_mir_*.cpp）
// ============================================================

void StackCBackend::Impl::emitNode(const MIRNode* node, codegen::CSourceStream& out)
{
    switch (node->kind) {
    case MIRKind::LABEL:
        out.append(labelIdent(labelIndex.at(static_cast<const MIRLabel*>(node)->labelNode)))
            .append(":");
        out.newLine();
        out.emitLine(";");
        break;
    case MIRKind::CONSTANT: emitConstant(static_cast<const MIRConstant*>(node), out); break;
    case MIRKind::LOCAL_LOAD: emitLocalLoad(static_cast<const MIRLocalLoad*>(node), out); break;
    case MIRKind::LOCAL_STORE: emitLocalStore(static_cast<const MIRLocalStore*>(node), out); break;
    case MIRKind::IINC: emitIinc(static_cast<const MIRIinc*>(node), out); break;
    case MIRKind::STACK_DUP: out.emitLine("stack[sp + 1] = stack[sp]; sp++;"); break;
    case MIRKind::STACK_DUP_X1:
        out.emitLine("{");
        out.indent();
        out.emitLine("union jvalue v1 = stack[sp];");
        out.emitLine("union jvalue v2 = stack[sp - 1];");
        out.emitLine("stack[sp - 1] = v1;");
        out.emitLine("stack[sp] = v2;");
        out.emitLine("stack[sp + 1] = v1;");
        out.emitLine("sp++;");
        out.dedent();
        out.emitLine("}");
        break;
    case MIRKind::STACK_POP: out.emitLine("sp--;"); break;
    case MIRKind::STACK_POP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        out.appendLineStart("sp -= ").appendInt(stackOpTopIsCategory2(op) ? 1 : 2).append(";");
        out.newLine();
        break;
    }
    case MIRKind::STACK_SWAP:
    {
        out.emitLine("{");
        out.indent();
        out.emitLine("union jvalue tmp = stack[sp];");
        out.emitLine("stack[sp] = stack[sp - 1];");
        out.emitLine("stack[sp - 1] = tmp;");
        out.dedent();
        out.emitLine("}");
        break;
    }
    case MIRKind::STACK_DUP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out.emitLine("stack[sp + 1] = stack[sp]; sp++;");
        }
        else {
            out.emitLine("stack[sp + 1] = stack[sp - 1];");
            out.emitLine("stack[sp + 2] = stack[sp];");
            out.emitLine("sp += 2;");
        }
        break;
    }
    case MIRKind::STACK_DUP2_X1:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v2;");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        break;
    }
    case MIRKind::STACK_DUP2_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            if (stackOpTopIsCategory2(op, 1)) {
                out.emitLine("{");
                out.indent();
                out.emitLine("union jvalue v1 = stack[sp];");
                out.emitLine("union jvalue v2 = stack[sp - 1];");
                out.emitLine("stack[sp - 1] = v1;");
                out.emitLine("stack[sp] = v2;");
                out.emitLine("stack[sp + 1] = v1;");
                out.emitLine("sp++;");
                out.dedent();
                out.emitLine("}");
            }
            else {
                out.emitLine("{");
                out.indent();
                out.emitLine("union jvalue v1 = stack[sp];");
                out.emitLine("union jvalue v2 = stack[sp - 1];");
                out.emitLine("union jvalue v3 = stack[sp - 2];");
                out.emitLine("stack[sp - 2] = v1;");
                out.emitLine("stack[sp - 1] = v3;");
                out.emitLine("stack[sp] = v2;");
                out.emitLine("stack[sp + 1] = v1;");
                out.emitLine("sp++;");
                out.dedent();
                out.emitLine("}");
            }
        }
        else if (stackOpTopIsCategory2(op, 2)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v2;");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("union jvalue v4 = stack[sp - 3];");
            out.emitLine("stack[sp - 3] = v2;");
            out.emitLine("stack[sp - 2] = v1;");
            out.emitLine("stack[sp - 1] = v4;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        break;
    }
    case MIRKind::STACK_DUP_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op, 1)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v1;");
            out.emitLine("stack[sp - 1] = v3;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
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
    case MIRKind::RETURN: out.append(renderStackCSnippet(StackCSnippet::ReturnVoid, {})); break;
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
                                                        codegen::CSourceStream& out,
                                                        const std::string&  dispatchTarget)
{
    currentExceptionDispatchTarget = dispatchTarget;
    emitNode(node, out);
    currentExceptionDispatchTarget.clear();
    if (!dispatchTarget.empty() && isProtectedByExceptionHandler(node)) {
        out.appendLineStart("if ((*env)->ExceptionCheck(env)) goto ")
            .append(dispatchTarget).append(";");
        out.newLine();
    }
}

}   // namespace mj2x

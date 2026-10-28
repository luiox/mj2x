/// @file stackc_registration.cpp
/// @brief StackC 注册链：hidden clinit / interface default 代理类生成与
///        RegisterNatives 发射（从 stackc_backend.cpp 单体拆出）。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

#include "mj2x/core/jvm_opcodes.hpp"

#include "mj2x/backend/dom_inject.hpp"
#include "mj2x/codegen/code_snippet.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include <cstdio>


namespace mj2x {

using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::cStringLiteral;
using codegen::registerFunctionName;

std::string hiddenClinitClassNameFor(const std::string& ownerClassName)
{
    const auto slash      = ownerClassName.find_last_of('/');
    const auto simpleName = "__mj2xHiddenClinit" + hex64(fnv1a64(ownerClassName));
    if (slash == std::string::npos) return simpleName;
    return ownerClassName.substr(0, slash + 1) + simpleName;
}

/// @brief native 代理壳类字节（批 6d DOM 构建）：单 native 静态方法、无体
///        无分支——native 方法不带 Code 属性（JVMS 4.7.3），RegisterNatives
///        运行时绑定与 maxStack/maxLocals 无关。
static std::vector<ca::u8> buildNativeProxyClassBytes(const std::string& hiddenClassName,
                                                      const std::string& methodName,
                                                      const std::string& methodDesc,
                                                      ca::u32            visibility)
{
    auto cf        = std::make_unique<jbc::core::ClassFile>();
    cf->major      = 52;   // V1_8
    cf->access     = Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cf->thisClass  = cf->cp.addClass(cf->cp.addUtf8(hiddenClassName));
    cf->superClass = cf->cp.addClass(cf->cp.addUtf8("java/lang/Object"));

    jbc::core::MemberInfo proxy;
    proxy.access = visibility | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE
                 | Opcodes::ACC_SYNTHETIC;
    proxy.nameCp = cf->cp.addUtf8(methodName);
    proxy.descCp = cf->cp.addUtf8(methodDesc);
    cf->methods.push_back(std::move(proxy));

    auto written = jbc::core::writeClass(*cf);
    if (written.is_err()) {
        throw std::runtime_error("stackc_registration: proxy class writeClass failed");
    }
    return std::move(written).unwrap();
}

std::vector<uint8_t> makeHiddenClinitClassBytes(const std::string& hiddenClassName,
                                                const std::string& hiddenMethodName)
{
    return buildNativeProxyClassBytes(hiddenClassName, hiddenMethodName, kHiddenClinitMethodDesc,
                                      Opcodes::ACC_PRIVATE);
}

std::vector<uint8_t> makeHiddenInterfaceDefaultClassBytes(const std::string& hiddenClassName,
                                                          const std::string& hiddenMethodName,
                                                          const std::string& hiddenMethodDesc)
{
    return buildNativeProxyClassBytes(hiddenClassName, hiddenMethodName, hiddenMethodDesc,
                                      Opcodes::ACC_PUBLIC);
}

void StackCBackend::Impl::protectClassInitializer(StackCBackend& backend, CompilerClass& clazz,
                                                  const std::string& className)
{
    if (!protectedClinitClasses.insert(className).second) return;

    auto idIt = registrationClassIds.find(className);
    if (idIt == registrationClassIds.end()) return;

    // hidden clinit 编译（簿记面）：clinit 体此刻仍是原体（inject 未触碰），
    // convert 直读 DOM；hidden 名走幻影快照（compileMethod 的名字源 =
    // CompilerMethod.name）——不落任何权威，clinit 体在下方于 DOM 全量重建。
    std::string hiddenName;
    std::string hiddenFunctionName;
    {
        auto&       dom       = *clazz.dom;
        const auto  clinitIdx = dom_inject::findMethodInDom(dom, "<clinit>", "()V");
        bool        hasBody   = false;
        if (clinitIdx < dom.methods.size()) {
            auto codeRes = dom.decodeCode(dom.methods[clinitIdx]);
            if (codeRes.is_ok()) {
                auto* code = std::move(codeRes).unwrap();
                hasBody    = code != nullptr && !code->code.empty();
            }
        }

        if (hasBody) {
            hiddenName = "__mj2x_hidden_clinit_" + hex64(fnv1a64(className));

            CompilerMethod hiddenMethod;
            hiddenMethod.name             = hiddenName;
            hiddenMethod.desc             = "()V";
            hiddenMethod.access           = Opcodes::ACC_STATIC;
            hiddenMethod.markedForCompile = true;
            // blocks 在下方 compileMethod 同步消费完即弃，局部 arena 活过消费期即可（spec §6.4）。
            ca::str::Utf8StringArena converterArena;
            CodeConverter            converter(converterArena);
            auto                     blocks = converter.convert(dom, clinitIdx);
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
            if (!hiddenFunctionName.empty())
                registerHiddenClinitProxy(className, hiddenName, hiddenFunctionName);
        }
    }

    // clinit 重建（DOM 权威）：体 = [ldc classId, ldc thisClass, invokestatic
    // bridge, RETURN]；原体已编译为 hidden proxy（运行期由 wrapper 调用）。
    const ca::i32 classId = idIt->second;
    dom_inject::replaceMethodBody(
        clazz, className, "<clinit>", "()V",
        [className, classId](jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor) {
            (void)className;
            auto add = [&editor](jbc::ir::Insn in) {
                (void)editor.append(std::move(in));
            };
            add(jbc::ir::CodeEditor::loadConst(session.internInteger(classId)));
            // loadConst（索引 > 0xFF 自动升 LDC_W）：cpInsn 硬编码 LDC 在大常量池
            // 类（IntrinsicSurface 20 拼接站点）索引超 u8，导出 bad instructions。
            add(jbc::ir::CodeEditor::loadConst(session.internClass(session.backing().thisName())));
            jbc::ir::Insn call;
            call.op = jbc::core::Op::INVOKESTATIC;
            call.cp = session.internMethodRef(kRegisterBridgeClassName, kRegisterBridgeMethodName,
                                              kRegisterBridgeMethodDesc);
            add(std::move(call));
            add(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        },
        2, 0, ~static_cast<ca::u32>(Opcodes::ACC_NATIVE));
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

void StackCBackend::Impl::registerInterfaceDefaultProxy(CompilerClass& clazz,
                                                        const std::string& methodName,
                                                        const std::string& methodDesc,
                                                        const std::string& compiledFunctionName)
{
    const auto className = clazz.dom != nullptr ? std::string(clazz.dom->thisName()) : std::string();
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
    CompilerClass& clazz, const std::string& mnName, const std::string& mnDesc,
    const CHiddenInterfaceDefaultProxy& proxy)
{
    // 批 5：体重建走 DOM（replaceMethodBody）；批 6d：身份源 = 快照 name/desc。
    const std::string className(clazz.dom != nullptr ? std::string(clazz.dom->thisName()) : "");

    // 形参布局（slot/maxStack）按旧口径在会话外算好，回调内逐参 xLOAD。
    auto argTypes = Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(mnDesc.c_str()));
    int  slot     = 1;
    int  maxStack = 1;
    for (const auto& argType : argTypes) {
        const int width = primitiveStackSize(jvmSortToPrimitive(argType.getSort()));
        slot += width;
        maxStack += width;
    }

    dom_inject::replaceMethodBody(
        clazz, className, mnName, mnDesc,
        [&proxy, &argTypes](jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor) {
            auto add = [&editor](jbc::ir::Insn in) {
                (void)editor.append(std::move(in));
            };
            add(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
            int paramSlot = 1;
            for (const auto& argType : argTypes) {
                add(jbc::ir::CodeEditor::local(
                    static_cast<jbc::core::Op>(loadOpcodeForSort(argType.getSort())),
                    static_cast<ca::u32>(paramSlot)));
                paramSlot += primitiveStackSize(jvmSortToPrimitive(argType.getSort()));
            }
            jbc::ir::Insn call;
            call.op = jbc::core::Op::INVOKESTATIC;
            call.cp = session.internMethodRef(proxy.hiddenClassName, proxy.hiddenMethodName,
                                              proxy.hiddenMethodDesc);
            add(std::move(call));
            add(jbc::ir::CodeEditor::simple(
                static_cast<jbc::core::Op>(returnOpcodeForKind(methodReturnKind(proxy.javaDesc)))));
        },
        static_cast<ca::u16>(maxStack), static_cast<ca::u16>(slot),
        ~static_cast<ca::u32>(Opcodes::ACC_NATIVE));
}

void StackCBackend::Impl::emitRegistrationCode(codegen::CSourceStream& out) const
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

        src.emitLine("static const uint8_t " + proxy.classBytesName + "[] = {");
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
            src.emitLine("static const uint8_t " + proxy.classBytesName + "[] = {");
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
    if (stringsEncrypt && !stringPoolUnits.empty()) {
        // --strings-encrypt：JNI_OnLoad 内一次性批量解密字符串池（at-rest 密文）。
        // JNI_OnLoad 由 JVM 保证单线程执行且先于任何 RegisterNatives/native 方法，
        // 无需调用点同步；池解密后生命周期覆盖整个执行期。
        src.emitLine("/* mj2x strings-encrypt: batch string pool decrypt (ciphertext at-rest) */");
        std::string decrypt = "mj2x_stackc_string_pool_decrypt(_mj2x_string_pool, ";
        decrypt += std::to_string(stringPoolUnits.size());
        decrypt += "u, 0x";
        char seedText[16];
        std::snprintf(seedText, sizeof(seedText), "%08x", stringPoolSeed());
        decrypt += seedText;
        decrypt += "u);";
        src.emitLine(decrypt);
    }
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

    out.append(src.str());
}

}   // namespace mj2x

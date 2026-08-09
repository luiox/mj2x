/// @file stackc_mir_objects.cpp
/// @brief StackC 对象/数组/字段发射器 + 无锁 CAS 的 field/method ID 缓存。
///
/// @details
/// ID 缓存从「owner monitor 双检锁」改为 jnic370 形态的「无锁 CAS + 快路径」：
/// 快路径 `_mj2x_cache_load` 是无锁读，慢路径 Get*ID 成功后用
/// `_mj2x_cache_cas` 发布，CAS 输家复用赢家结果。MSVC 走 Interlocked，
/// GCC/Clang/Zig 走 __atomic（平台宏定义在 stackc_runtime_prelude.cpp）。
///
/// 缩进约定：调用方保证 out 处于 2 层（8 空格），内部块用 indent()/dedent()。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

namespace mj2x {

using namespace jbytecode::core;
using codegen::cStringLiteral;
using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;

void StackCBackend::Impl::emitFieldIdCache(const std::string& cacheName,
                                           const std::string& localName,
                                           const std::string& classExpr, const std::string& name,
                                           const std::string& desc, bool isStatic,
                                           codegen::CSourceStream& out)
{
    const std::string staticPrefix = isStatic ? "Static" : "";
    out.emitLine("if ((*env)->ExceptionCheck(env)) {");
    out.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
    out.appendLineStart("if (").append(classExpr).append(" == NULL) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass ex_class = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/NoClassDefFoundError\")")).append(";");
    out.newLine();
    out.emitLine("(*env)->ThrowNew(env, ex_class, \"class lookup failed\");");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
    out.emitLine("static jfieldID " + cacheName + " = NULL;");
    out.emitLine("jfieldID " + localName + " = NULL;");
    out.appendLineStart(localName).append(" = (jfieldID)_mj2x_cache_load((void* volatile*)&")
        .append(cacheName).append(");");
    out.newLine();
    out.appendLineStart("if (_MJ2X_UNLIKELY(").append(localName).append(" == NULL)) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jfieldID loaded = (*env)->Get").append(staticPrefix)
        .append("FieldID(env, ").append(classExpr).append(", ").append(cStringLiteral(name))
        .append(", ").append(cStringLiteral(desc)).append(");");
    out.newLine();
    out.emitLine("if (loaded != NULL) {");
    out.indent();
    out.appendLineStart("void* previous = _mj2x_cache_cas((void* volatile*)&").append(cacheName)
        .append(", NULL, loaded);");
    out.newLine();
    out.appendLineStart(localName).append(" = (jfieldID)(previous != NULL ? previous : loaded);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.dedent();
    out.emitLine("}");
    emitPendingExceptionCheck(out, 8);
}

void StackCBackend::Impl::emitMethodIdCache(const std::string& cacheName,
                                            const std::string& localName,
                                            const std::string& classExpr, const std::string& name,
                                            const std::string& desc, bool isStatic,
                                            codegen::CSourceStream& out)
{
    const std::string staticPrefix = isStatic ? "Static" : "";
    out.emitLine("if ((*env)->ExceptionCheck(env)) {");
    out.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
    out.appendLineStart("if (").append(classExpr).append(" == NULL) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass ex_class = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/NoClassDefFoundError\")")).append(";");
    out.newLine();
    out.emitLine("(*env)->ThrowNew(env, ex_class, \"class lookup failed\");");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
    out.emitLine("static jmethodID " + cacheName + " = NULL;");
    out.emitLine("jmethodID " + localName + " = NULL;");
    out.appendLineStart(localName).append(" = (jmethodID)_mj2x_cache_load((void* volatile*)&")
        .append(cacheName).append(");");
    out.newLine();
    out.appendLineStart("if (_MJ2X_UNLIKELY(").append(localName).append(" == NULL)) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jmethodID loaded = (*env)->Get").append(staticPrefix)
        .append("MethodID(env, ").append(classExpr).append(", ").append(cStringLiteral(name))
        .append(", ").append(cStringLiteral(desc)).append(");");
    out.newLine();
    out.emitLine("if (loaded != NULL) {");
    out.indent();
    out.appendLineStart("void* previous = _mj2x_cache_cas((void* volatile*)&").append(cacheName)
        .append(", NULL, loaded);");
    out.newLine();
    out.appendLineStart(localName).append(" = (jmethodID)(previous != NULL ? previous : loaded);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.dedent();
    out.emitLine("}");
    emitPendingExceptionCheck(out, 8);
}

/// @brief 发射「jclass lc = 缓存的类引用」（对标 JNIC cached_c_N）。
/// @details self 类直接取 `_mj2x_owner_class`（clazz 参数即该类，零开销）；其余类
///          走 static jclass + 无锁 CAS 缓存，首次执行 `_mj2x_find_class` 后
///          以 NewGlobalRef 固化，后续路径只读缓存 slot。失败抛
///          NoClassDefFoundError（dispatch 模式跳转 handler，否则 return）。
void StackCBackend::Impl::emitClassRefCache(const std::string& className,
                                            codegen::CSourceStream& out)
{
    std::string name = className;
    if (name.size() >= 2 && name.front() == 'L' && name.back() == ';') {
        name = name.substr(1, name.size() - 2);
    }
    // 注：不做 owner==self 优化——`_mj2x_owner_class` 是运行期实例类（多态下可能
    // 是被继承的 Sub 类），而 owner 是编译期静态解析的类，两者可能不同。
    const std::string cacheName = "_mj2x_cc_" + hex64(fnv1a64(name));
    out.emitLine("static jclass " + cacheName + " = NULL;");
    out.emitLine("jclass lc = NULL;");
    out.appendLineStart("lc = (jclass)_mj2x_cache_load((void* volatile*)&")
        .append(cacheName).append(");");
    out.newLine();
    out.appendLineStart("if (_MJ2X_UNLIKELY(lc == NULL)) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass loaded = (jclass)").append(trackLocalRefExpr(
        "_mj2x_find_class(env, _mj2x_owner_class, \"" + name + "\")")).append(";");
    out.newLine();
    out.appendLineStart("if (loaded != NULL) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass global = (jclass)(*env)->NewGlobalRef(env, loaded);");
    out.newLine();
    out.appendLineStart("(*env)->DeleteLocalRef(env, loaded);");
    out.newLine();
    out.appendLineStart("if (global != NULL) {");
    out.newLine();
    out.indent();
    out.appendLineStart("void* previous = _mj2x_cache_cas((void* volatile*)&").append(cacheName)
        .append(", NULL, global);");
    out.newLine();
    out.appendLineStart("lc = (jclass)(previous != NULL ? previous : global);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.dedent();
    out.emitLine("}");
    out.dedent();
    out.emitLine("}");
    out.appendLineStart("if (_MJ2X_UNLIKELY(lc == NULL)) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass ex_class = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/NoClassDefFoundError\")")).append(";");
    out.newLine();
    out.emitLine("(*env)->ThrowNew(env, ex_class, \"class lookup failed\");");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitNewObject(const MIRNewObject* node, codegen::CSourceStream& out)
{
    // NEW 只分配对象、压栈；构造由后续 INVOKESPECIAL <init>（走 emitInvoke 的
    // CallNonvirtualVoidMethod 路径）完成。用 AllocObject 解耦分配与初始化，
    // 避免 emitNewObject 推断构造器签名（MIRNewObject 不带 ctor desc）。
    out.emitLine("{");
    out.indent();
    emitClassRefCache(node->typeDesc.to_std_string(), out);
    out.appendLineStart("stack[++sp].l = ")
        .append(trackLocalRefExpr("(*env)->AllocObject(env, lc)")).append(";");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitNewArray(const MIRNewArray* node, codegen::CSourceStream& out)
{
    if (node->componentType == PrimitiveKind::OBJECT) {
        out.appendLineStart("stack[sp].l = (*env)->NewObjectArray(env, stack[sp].i, ");
        if (node->typeDesc.is_empty()) {
            out.append("(jclass)").append(trackLocalRefExpr(
                "_mj2x_find_class(env, _mj2x_owner_class, \"java/lang/Object\")")).append(";");
        }
        else {
            out.append(classExprForDescriptor(node->typeDesc.to_std_string()));
        }
        out.append(", NULL);");
        out.newLine();
    }
    else {
        out.appendLineStart("stack[sp].l = ").append(trackLocalRefExpr(
            "(jobject)(*env)->" + std::string(primitiveToNewArrayFn(node->componentType)) +
            "(env, stack[sp].i)")).append(";");
        out.newLine();
    }
}

void StackCBackend::Impl::emitMultiNewArray(const MIRMultiNewArray* node, codegen::CSourceStream& out)
{
    out.emitLine("{");
    out.indent();
    out.appendLineStart("jint dims[").appendInt(node->dims).append("];");
    out.newLine();
    for (int i = node->dims - 1; i >= 0; --i) {
        out.appendLineStart("dims[").appendInt(i).append("] = stack[sp--].i;");
        out.newLine();
    }
    out.appendLineStart("stack[++sp].l = ").append(trackLocalRefExpr(
        "_mj2x_new_multi_array(env, _mj2x_owner_class, \"" + node->typeDesc.to_std_string() +
        "\", dims, " + std::to_string(node->dims) + ")")).append(";");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitArrayLength(const MIRArrayLength* node, codegen::CSourceStream& out)
{
    (void)node;
    emitNullPointerTrap("stack[sp].l", out);
    out.append(renderStackCSnippet(StackCSnippet::ArrayLength, {}));
}

void StackCBackend::Impl::emitArrayLoad(const MIRArrayLoad* node, codegen::CSourceStream& out)
{
    emitNullPointerTrap("stack[sp - 1].l", out);
    if (node->componentType == PrimitiveKind::OBJECT) {
        out.append(renderStackCSnippet(
            StackCSnippet::ObjectArrayLoad,
            {{"value",
              trackLocalRefExpr("(*env)->GetObjectArrayElement(env, (jobjectArray)stack[sp - 1].l, "
                                "stack[sp].i)")}}));
        return;
    }
    if (node->componentType == PrimitiveKind::BYTE) {
        out.emitLine("{");
        out.indent();
        out.appendLineStart("jclass boolean_array_class = (jclass)").append(
            trackLocalRefExpr("(*env)->FindClass(env, \"[Z\")")).append(";");
        out.newLine();
        out.emitLine("if ((*env)->IsInstanceOf(env, stack[sp - 1].l, boolean_array_class)) {");
        out.indent();
        out.emitLine("jboolean value;");
        out.emitLine("(*env)->GetBooleanArrayRegion(env, (jbooleanArray)stack[sp - 1].l, "
                     "stack[sp].i, 1, &value);");
        out.emitLine("stack[sp - 1].i = (jint)value;");
        out.dedent();
        out.emitLine("} else {");
        out.indent();
        out.emitLine("jbyte value;");
        out.emitLine("(*env)->GetByteArrayRegion(env, (jbyteArray)stack[sp - 1].l, "
                     "stack[sp].i, 1, &value);");
        out.emitLine("stack[sp - 1].i = (jint)value;");
        out.dedent();
        out.emitLine("}");
        out.dedent();
        out.emitLine("}");
        out.emitLine("sp--;");
        return;
    }
    const char* suffix = primitiveToJNISuffix(node->componentType);
    const char* type   = primitiveToJNIType(node->componentType);
    out.append(renderStackCSnippet(
        StackCSnippet::PrimitiveArrayLoad,
        {{"type", type},
         {"suffix", suffix},
         {"array_type", primitiveToJNIArrayType(node->componentType)},
         {"acc", stackValueAccessor(node->componentType)},
         {"target_type", narrowOnOperandStack(node->componentType) ? "jint" : type}}));
}

void StackCBackend::Impl::emitArrayStore(const MIRArrayStore* node, codegen::CSourceStream& out)
{
    emitNullPointerTrap("stack[sp - 2].l", out);
    if (node->componentType == PrimitiveKind::OBJECT) {
        out.append(renderStackCSnippet(StackCSnippet::ObjectArrayStore, {}));
        return;
    }
    if (node->componentType == PrimitiveKind::BYTE) {
        out.emitLine("{");
        out.indent();
        out.appendLineStart("jclass boolean_array_class = (jclass)").append(
            trackLocalRefExpr("(*env)->FindClass(env, \"[Z\")")).append(";");
        out.newLine();
        out.emitLine("if ((*env)->IsInstanceOf(env, stack[sp - 2].l, boolean_array_class)) {");
        out.indent();
        out.emitLine("jboolean value = (jboolean)(stack[sp].i & 1);");
        out.emitLine("(*env)->SetBooleanArrayRegion(env, (jbooleanArray)stack[sp - 2].l, "
                     "stack[sp - 1].i, 1, &value);");
        out.dedent();
        out.emitLine("} else {");
        out.indent();
        out.emitLine("jbyte value = (jbyte)stack[sp].i;");
        out.emitLine("(*env)->SetByteArrayRegion(env, (jbyteArray)stack[sp - 2].l, stack[sp "
                     "- 1].i, 1, &value);");
        out.dedent();
        out.emitLine("}");
        out.dedent();
        out.emitLine("}");
        out.emitLine("sp -= 3;");
        return;
    }
    const char* suffix = primitiveToJNISuffix(node->componentType);
    const char* type   = primitiveToJNIType(node->componentType);
    out.append(renderStackCSnippet(StackCSnippet::PrimitiveArrayStore,
                                   {{"type", type},
                                    {"suffix", suffix},
                                    {"array_type", primitiveToJNIArrayType(node->componentType)},
                                    {"acc", stackValueAccessor(node->componentType)}}));
}

void StackCBackend::Impl::emitFieldGet(const MIRFieldGet* node, codegen::CSourceStream& out)
{
    auto              pk      = jvmDescToPrimitive(node->desc);
    const char*       suffix = primitiveToJNISuffix(pk);
    const char*       acc    = stackValueAccessor(pk);
    const std::string owner  = node->owner.to_std_string();
    const std::string name   = node->name.to_std_string();
    const std::string desc   = node->desc.to_std_string();
    const auto        fidCache = cacheIdentifier("fid", owner + "." + name + ":" + desc);
    out.emitLine("{");
    out.indent();
    emitClassRefCache(owner, out);
    emitFieldIdCache(fidCache, "fid_local", "lc", name, desc, false, out);
    emitNullPointerTrap("stack[sp].l", out);
    out.appendLineStart("stack[sp]").append(acc).append(" = (")
        .append(narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk))
        .append(")(*env)->Get").append(suffix).append("Field(env, stack[sp].l, fid_local);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitFieldPut(const MIRFieldPut* node, codegen::CSourceStream& out)
{
    auto              pk      = jvmDescToPrimitive(node->desc);
    const char*       suffix  = primitiveToJNISuffix(pk);
    const char*       acc     = stackValueAccessor(pk);
    const std::string owner   = node->owner.to_std_string();
    const std::string name    = node->name.to_std_string();
    const std::string desc    = node->desc.to_std_string();
    const auto        fidCache = cacheIdentifier("fid", owner + "." + name + ":" + desc);
    out.emitLine("{");
    out.indent();
    emitClassRefCache(owner, out);
    emitFieldIdCache(fidCache, "fid_local", "lc", name, desc, false, out);
    emitNullPointerTrap("stack[sp - 1].l", out);
    out.appendLineStart("(*env)->Set").append(suffix)
        .append("Field(env, stack[sp - 1].l, fid_local, (")
        .append(primitiveToJNIType(pk)).append(")stack[sp]").append(acc).append(");");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.emitLine("sp -= 2;");
}

void StackCBackend::Impl::emitStaticGet(const MIRStaticGet* node, codegen::CSourceStream& out)
{
    auto              pk     = jvmDescToPrimitive(node->desc);
    const char*       suffix = primitiveToJNISuffix(pk);
    const char*       acc    = stackValueAccessor(pk);
    const std::string owner  = node->owner.to_std_string();
    const std::string name   = node->name.to_std_string();
    const std::string desc   = node->desc.to_std_string();
    const auto        fidCache =
        cacheIdentifier("sfid", owner + "." + name + ":" + desc);
    out.emitLine("{");
    out.indent();
    emitClassRefCache(owner, out);
    emitFieldIdCache(fidCache, "fid_local", "lc", name, desc, true, out);
    out.appendLineStart("stack[++sp]").append(acc).append(" = (")
        .append(narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk))
        .append(")(*env)->GetStatic").append(suffix).append("Field(env, lc, fid_local);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitStaticPut(const MIRStaticPut* node, codegen::CSourceStream& out)
{
    auto              pk     = jvmDescToPrimitive(node->desc);
    const char*       suffix = primitiveToJNISuffix(pk);
    const char*       acc    = stackValueAccessor(pk);
    const std::string owner  = node->owner.to_std_string();
    const std::string name   = node->name.to_std_string();
    const std::string desc   = node->desc.to_std_string();
    const auto        fidCache =
        cacheIdentifier("sfid", owner + "." + name + ":" + desc);
    out.emitLine("{");
    out.indent();
    emitClassRefCache(owner, out);
    emitFieldIdCache(fidCache, "fid_local", "lc", name, desc, true, out);
    out.appendLineStart("(*env)->SetStatic").append(suffix)
        .append("Field(env, lc, fid_local, (")
        .append(primitiveToJNIType(pk)).append(")stack[sp]").append(acc).append(");");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.emitLine("sp--;");
}

void StackCBackend::Impl::emitCheckCast(const MIRCheckCast* node, codegen::CSourceStream& out)
{
    out.appendLineStart("if (stack[sp].l != NULL && !(*env)->IsInstanceOf(env, stack[sp].l, ")
        .append(classExprForDescriptor(node->typeDesc.to_std_string())).append(")) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass ex_class = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/ClassCastException\")")).append(";");
    out.newLine();
    out.appendLineStart("(*env)->ThrowNew(env, ex_class, \"")
        .append(node->typeDesc.to_std_string()).append("\");");
    out.newLine();
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitInstanceOf(const MIRInstanceOf* node, codegen::CSourceStream& out)
{
    out.appendLineStart("stack[sp].i = (*env)->IsInstanceOf(env, stack[sp].l, ")
        .append(classExprForDescriptor(node->typeDesc.to_std_string())).append(");");
    out.newLine();
}

}   // namespace mj2x


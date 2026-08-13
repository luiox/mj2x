/// @file stackc_mir_invoke.cpp
/// @brief StackC 方法调用 / invokedynamic 发射器（从 stackc_backend.cpp 拆出）。
///
/// @details 缩进约定：调用时 out 处于方法体层（1 层），emitInvokeDynamic 内部
///          主体块 indent 到 2 层，嵌套块用 indent()/dedent() 管理。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

#include <iomanip>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::analysis;
using codegen::cStringLiteral;
using codegen::formatFloatLiteral;
using codegen::fnv1a64;
using codegen::hex64;

void StackCBackend::Impl::emitInvoke(const MIRInvoke* node, codegen::CSourceStream& out)
{
    const ca::str::Utf8StringRef descRef = node->desc;
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor());
    const std::string owner = node->owner.to_std_string();
    const std::string name  = node->name.to_std_string();
    const std::string desc  = node->desc.to_std_string();

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
                                             owner + "." + name + ":" + desc +
                                                 ":" + std::to_string(node->invokeOpcode));

    // 整段放 block 内：先查 jclass/jmethodID，再调用。返回值写入接收者槽（非 void）。
    out.emitLine("{");
    out.indent();
    emitClassRefCache(owner, out);
    emitMethodIdCache(methodCache, "mid_local", "lc", name, desc, isStatic, out);
    if (!isStatic) {
        emitNullPointerTrap("stack[" + recvExpr + "].l", out);
    }
    if (!isStatic) {
        if (retType != PrimitiveKind::VOID) {
            out.appendLineStart("stack[").append(recvExpr).append("]")
                .append(stackValueAccessor(retType)).append(" = ");
            out.append("(*env)->").append(callPrefix).append(retSuffix)
                .append("Method(env, stack[").append(recvExpr).append("].l");
            // CallNonvirtual* 需要 (env, obj, jclass, methodID, ...)：在 obj 后插 jclass lc。
            if (isNonvirtual) out.append(", lc");
            out.append(", mid_local");
        }
        else {
            out.appendLineStart("(*env)->").append(callPrefix).append(retSuffix)
                .append("Method(env, stack[").append(recvExpr).append("].l");
            if (isNonvirtual) out.append(", lc");
            out.append(", mid_local");
        }
    }
    else {
        if (retType != PrimitiveKind::VOID) {
            if (argCount == 0) {
                out.appendLineStart("stack[++sp]").append(stackValueAccessor(retType)).append(" = ");
            }
            else {
                out.appendLineStart("stack[sp - ").appendInt(resultIndex).append("]")
                    .append(stackValueAccessor(retType)).append(" = ");
            }
        }
        else {
            out.appendLineStart("");
        }
        out.append("(*env)->").append(callPrefix).append(retSuffix).append("Method(env, lc, mid_local");
    }
    // 参数列表：arg0 在 sp-argCount+1，依次往后；静态/实例共享同一参数区。
    for (int i = 0; i < argCount; ++i) {
        auto        pka        = jvmSortToPrimitive(argTypes[static_cast<size_t>(i)].getSort());
        int         distFromSp = argCount - 1 - i;
        out.append(", stack[sp - ").appendInt(distFromSp).append("]").append(stackValueAccessor(pka));
    }
    out.append(");");
    out.newLine();
    emitPendingExceptionCheck(out);
    out.dedent();
    out.emitLine("}");

    if (isStatic) {
        if (retType == PrimitiveKind::VOID) {
            if (argCount > 0) {
                out.appendLineStart("sp -= ").appendInt(argCount).append(";");
                out.newLine();
            }
        }
        else if (argCount > 0) {
            out.appendLineStart("sp -= ").appendInt(argCount - 1).append(";");
            out.newLine();
        }
    }
    else {
        int popCount = argCount + 1;
        if (retType != PrimitiveKind::VOID) popCount -= 1;
        if (popCount > 0) {
            out.appendLineStart("sp -= ").appendInt(popCount).append(";");
            out.newLine();
        }
    }
}

void StackCBackend::Impl::emitInvokeDynamic(const MIRInvokeDynamic* node, codegen::CSourceStream& out)
{
    const ca::str::Utf8StringRef descRef  = node->desc;
    auto                         argTypes = Type::getArgumentTypes(descRef);
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor());
    const int argCount = static_cast<int>(argTypes.size());

    auto emitExceptionCheck = [&]() {
        out.emitLine("if (_MJ2X_UNLIKELY((*env)->ExceptionCheck(env))) {");
        out.indent();
        if (!currentExceptionDispatchTarget.empty()) {
            out.emitLine("goto " + currentExceptionDispatchTarget + ";");
        }
        else {
            emitExceptionReturn(currentMethodDesc, out);
        }
        out.dedent();
        out.emitLine("}");
    };

    auto emitStringObject = [&](const std::string& varName, const std::string& value) {
        out.appendLineStart("jstring ").append(varName).append(" = (jstring)").append(
            trackLocalRefExpr("(*env)->NewStringUTF(env, " + cStringLiteral(value) + ")")).append(";");
        out.newLine();
    };

    auto emitClassObjectForDescriptor = [&](const std::string& varName, const std::string& desc) {
        const auto kind = jvmDescToPrimitive(desc);
        if (kind != PrimitiveKind::OBJECT || desc == "V") {
            out.appendLineStart("jclass ").append(varName).append("_wrapper = (jclass)").append(
                trackLocalRefExpr("(*env)->FindClass(env, \"" + std::string(boxedClassName(kind)) +
                                  "\")")).append(";");
            out.newLine();
            emitCLine(out,
                      2,
                      "jfieldID " + varName + "_type_fid = (*env)->GetStaticFieldID(env, " +
                          varName + "_wrapper, \"TYPE\", \"Ljava/lang/Class;\");");
            out.appendLineStart("jclass ").append(varName).append(" = (jclass)").append(
                trackLocalRefExpr("(*env)->GetStaticObjectField(env, " + varName +
                                  "_wrapper, " + varName + "_type_fid)")).append(";");
            out.newLine();
        }
        else {
            out.appendLineStart("jclass ").append(varName).append(" = ")
                .append(classExprForDescriptor(desc));
            out.newLine();
        }
    };

    auto emitMethodType = [&](const std::string& varName, const std::string& methodDesc) {
        emitStringObject(varName + "_desc", methodDesc);
        out.appendLineStart("jclass ").append(varName).append("_class_cls = (jclass)").append(
            trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/Class\")")).append(";");
        out.newLine();
        emitCLine(out,
                  2,
                  "jmethodID " + varName + "_loader_mid = (*env)->GetMethodID(env, " + varName +
                      "_class_cls, \"getClassLoader\", \"()Ljava/lang/ClassLoader;\");");
        out.appendLineStart("jobject ").append(varName).append("_loader = ").append(
            trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_owner_class, " +
                              varName + "_loader_mid)")).append(";");
        out.newLine();
        emitExceptionCheck();
        out.appendLineStart("jclass ").append(varName).append("_mt_cls = (jclass)").append(
            trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodType\")")).append(";");
        out.newLine();
        emitCLine(
            out,
            2,
            "jmethodID " + varName + "_from_desc_mid = (*env)->GetStaticMethodID(env, " + varName +
                "_mt_cls, \"fromMethodDescriptorString\", "
                "\"(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;\");");
        out.appendLineStart("jobject ").append(varName).append(" = ").append(
            trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, " + varName + "_mt_cls, " +
                              varName + "_from_desc_mid, " + varName + "_desc, " + varName +
                              "_loader)")).append(";");
        out.newLine();
        emitExceptionCheck();
    };

    auto emitBoxPrimitiveExpr =
        [&](const std::string& varName, PrimitiveKind kind, const std::string& valueExpr) {
            out.appendLineStart("jclass ").append(varName).append("_box_cls = (jclass)").append(
                trackLocalRefExpr("(*env)->FindClass(env, \"" +
                                  std::string(boxedClassName(kind)) + "\")")).append(";");
            out.newLine();
            emitCLine(out,
                      2,
                      "jmethodID " + varName + "_valueof_mid = (*env)->GetStaticMethodID(env, " +
                          varName + "_box_cls, \"valueOf\", " +
                          cStringLiteral(boxedValueOfDesc(kind)) + ");");
            out.appendLineStart("jobject ").append(varName).append(" = ").append(
                trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, " + varName + "_box_cls, " +
                                  varName + "_valueof_mid, " + valueExpr + ")")).append(";");
            out.newLine();
        };

    auto emitBoxStackValue =
        [&](const std::string& varName, PrimitiveKind kind, const std::string& stackIndexExpr) {
            if (kind == PrimitiveKind::OBJECT) {
                out.appendLineStart("jobject ").append(varName).append(" = stack[")
                    .append(stackIndexExpr).append("].l;");
                out.newLine();
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
        out.appendLineStart("jobject ").append(varName).append(" = NULL;");
        out.newLine();
        out.emitLine("{");
        out.indent();
        out.appendLineStart("jclass ").append(varName).append("_lookup_cls = (jclass)").append(
            trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodHandles$Lookup\")")).append(";");
        out.newLine();
        out.appendLineStart("jclass ").append(varName).append("_owner_cls = (jclass)").append(
            trackLocalRefExpr("_mj2x_find_class(env, _mj2x_owner_class, \"" + owner + "\")")).append(";");
        out.newLine();
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
        out.appendLineStart(varName).append(" = ").append(
            trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_indy_lookup, " + varName +
                              "_find_mid, " + args + ")")).append(";");
        out.newLine();
        emitExceptionCheck();
        out.dedent();
        out.emitLine("}");
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
            emitBoxPrimitiveExpr(varName, PrimitiveKind::FLOAT, formatFloatLiteral(*p, 9) + "f");
        }
        else if (auto* p = value.as<double>()) {
            emitBoxPrimitiveExpr(varName, PrimitiveKind::DOUBLE, formatFloatLiteral(*p, 17));
        }
        else {
            out.appendLineStart("jobject ").append(varName).append(" = NULL;");
            out.newLine();
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
            return formatFloatLiteral(*p, 9) + "f";
        }
        if (auto* p = value.as<double>()) {
            return formatFloatLiteral(*p, 17);
        }
        if (primitive == PrimitiveKind::BOOLEAN) return "JNI_FALSE";
        return "0";
    };

    out.emitLine("{");
    out.indent();
    out.appendLineStart("jclass _mj2x_indy_object_cls = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/Object\")")).append(";");
    out.newLine();
    out.appendLineStart("jobjectArray _mj2x_indy_args = (jobjectArray)").append(
        trackLocalRefExpr("(*env)->NewObjectArray(env, " + std::to_string(argCount) +
                          ", _mj2x_indy_object_cls, NULL)")).append(";");
    out.newLine();
    for (int i = 0; i < argCount; ++i) {
        const auto        kind = jvmSortToPrimitive(argTypes[static_cast<size_t>(i)].getSort());
        const int         distFromSp = argCount - 1 - i;
        const std::string stackIndexExpr =
            distFromSp == 0 ? "sp" : "sp - " + std::to_string(distFromSp);
        emitBoxStackValue("_mj2x_indy_arg_" + std::to_string(i), kind, stackIndexExpr);
        out.appendLineStart("(*env)->SetObjectArrayElement(env, _mj2x_indy_args, ")
            .appendInt(i).append(", _mj2x_indy_arg_").appendInt(i).append(");");
        out.newLine();
    }
    if (argCount > 0) {
        out.appendLineStart("sp -= ").appendInt(argCount).append(";");
        out.newLine();
    }

    out.appendLineStart("jclass _mj2x_indy_mh_cls = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/invoke/MethodHandles\")")).append(";");
    out.newLine();
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_lookup_mid = (*env)->GetStaticMethodID(env, "
              "_mj2x_indy_mh_cls, \"lookup\", \"()Ljava/lang/invoke/MethodHandles$Lookup;\");");
    out.appendLineStart("jobject _mj2x_indy_base_lookup = ").append(trackLocalRefExpr(
        "(*env)->CallStaticObjectMethod(env, _mj2x_indy_mh_cls, _mj2x_indy_lookup_mid)")).append(";");
    out.newLine();
    emitExceptionCheck();
    out.emitLine("jobject _mj2x_indy_lookup = _mj2x_indy_base_lookup;");
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_private_lookup_mid = (*env)->GetStaticMethodID(env, "
              "_mj2x_indy_mh_cls, \"privateLookupIn\", "
              "\"(Ljava/lang/Class;Ljava/lang/invoke/MethodHandles$Lookup;)Ljava/lang/invoke/"
              "MethodHandles$Lookup;\");");
    out.emitLine("if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);");
    out.emitLine("if (_mj2x_indy_private_lookup_mid != NULL) {");
    out.indent();
    out.appendLineStart("jobject _mj2x_indy_private_lookup = ").append(trackLocalRefExpr(
        "(*env)->CallStaticObjectMethod(env, _mj2x_indy_mh_cls, "
        "_mj2x_indy_private_lookup_mid, _mj2x_owner_class, _mj2x_indy_base_lookup)")).append(";");
    out.newLine();
    out.emitLine("if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);");
    out.emitLine("else _mj2x_indy_lookup = _mj2x_indy_private_lookup;");
    out.dedent();
    out.emitLine("}");

    emitStringObject("_mj2x_indy_name", node->name.to_std_string());
    emitMethodType("_mj2x_indy_type", node->desc.to_std_string());

    if (!node->bsm.has_value()) {
        emitUnsupported("invokedynamic without bootstrap metadata", out);
        out.dedent();
        out.emitLine("}");
        return;
    }
    const auto&       bsm      = *node->bsm;
    const std::string bsmOwner = utf8RefToString(bsm.getOwner());
    const std::string bsmName  = utf8RefToString(bsm.getName());
    const std::string bsmDesc  = utf8RefToString(bsm.getDesc());
    const auto        indyTargetCache =
        cacheIdentifier("indy_target",
                        currentClassName + "." + currentMethodName + currentMethodDesc + ":" +
                            node->name.to_std_string() + ":" + node->desc.to_std_string() + ":" +
                            bsmOwner + "." + bsmName + ":" +
                            bsmDesc + ":" + std::to_string(node->bsmArgs.size()));

    out.appendLineStart("static jobject ").append(indyTargetCache).append(" = NULL;");
    out.newLine();
    out.emitLine("jobject _mj2x_indy_target = NULL;");
    out.appendLineStart("if (").append(indyTargetCache).append(" != NULL) {");
    out.newLine();
    out.indent();
    out.appendLineStart("_mj2x_indy_target = (*env)->NewLocalRef(env, ")
        .append(indyTargetCache).append(");");
    out.newLine();
    out.appendLineStart("if (_mj2x_indy_target == NULL) ").append(indyTargetCache).append(" = NULL;");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.emitLine("if (_mj2x_indy_target == NULL) {");
    out.indent();
    auto bsmArgTypes = Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(bsmDesc.c_str()));
    out.appendLineStart("jclass _mj2x_indy_bsm_cls = (jclass)").append(trackLocalRefExpr(
        "_mj2x_find_class(env, _mj2x_owner_class, \"" + bsmOwner + "\")")).append(";");
    out.newLine();
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
            out.appendLineStart("jobjectArray _mj2x_indy_bsm_varargs = (jobjectArray)").append(
                trackLocalRefExpr("(*env)->NewObjectArray(env, " + std::to_string(remaining) +
                                  ", _mj2x_indy_object_cls, NULL)")).append(";");
            out.newLine();
            for (size_t j = 0; j < remaining; ++j) {
                const std::string argVar = "_mj2x_indy_bsm_vararg_" + std::to_string(j);
                emitAnyAsObject(argVar, node->bsmArgs[bsmArgIndex + j]);
                out.appendLineStart("(*env)->SetObjectArrayElement(env, _mj2x_indy_bsm_varargs, ")
                    .appendInt(static_cast<int64_t>(j)).append(", ").append(argVar).append(");");
                out.newLine();
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

    out.appendLineStart("jobject _mj2x_indy_callsite = ").append(
        trackLocalRefExpr("(*env)->CallStaticObjectMethod(env, _mj2x_indy_bsm_cls, "
                          "_mj2x_indy_bsm_mid")).append(";");
    for (const auto& expr : bsmExprs) {
        out.append(", ").append(expr);
    }
    out.append(");");
    out.newLine();
    emitExceptionCheck();

    out.appendLineStart("jclass _mj2x_indy_callsite_cls = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/invoke/CallSite\")")).append(";");
    out.newLine();
    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_get_target_mid = (*env)->GetMethodID(env, "
              "_mj2x_indy_callsite_cls, \"getTarget\", \"()Ljava/lang/invoke/MethodHandle;\");");
    out.indent();
    out.appendLineStart("_mj2x_indy_target = ").append(trackLocalRefExpr(
        "(*env)->CallObjectMethod(env, _mj2x_indy_callsite, _mj2x_indy_get_target_mid)")).append(";");
    out.newLine();
    emitExceptionCheck();
    out.emitLine("if (_mj2x_indy_target != NULL) {");
    out.indent();
    out.appendLineStart("jobject _mj2x_indy_target_global = (*env)->NewGlobalRef(env, "
                        "_mj2x_indy_target);");
    out.newLine();
    out.appendLineStart("if (_mj2x_indy_target_global != NULL) ").append(indyTargetCache)
        .append(" = _mj2x_indy_target_global;");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    out.dedent();
    out.emitLine("}");

    out.appendLineStart("jclass _mj2x_indy_method_handle_cls = (jclass)").append(
        trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/invoke/MethodHandle\")")).append(";");
    out.newLine();
    const bool useOneObjectInvokeHelper =
        retType == PrimitiveKind::OBJECT && argCount == 1 &&
        (argTypes[0].getSort() == Type::OBJECT || argTypes[0].getSort() == Type::ARRAY);
    if (useOneObjectInvokeHelper) {
        out.appendLineStart("jclass _mj2x_indy_cloader_cls = (jclass)").append(trackLocalRefExpr(
            "(*env)->FindClass(env, \"mj2x/cloader/Loader\")")).append(";");
        out.newLine();
        emitCLine(out,
                  2,
                  "jmethodID _mj2x_indy_helper_mid = (*env)->GetStaticMethodID(env, "
                  "_mj2x_indy_cloader_cls, \"invokeMethodHandle1\", "
                  "\"(Ljava/lang/invoke/MethodHandle;Ljava/lang/Object;)Ljava/lang/Object;\");");
        out.emitLine("jobject _mj2x_indy_capture0 = stack[sp + 1].l;");
        out.appendLineStart("stack[++sp].l = ").append(trackLocalRefExpr(
            "(*env)->CallStaticObjectMethod(env, _mj2x_indy_cloader_cls, "
            "_mj2x_indy_helper_mid, _mj2x_indy_target, _mj2x_indy_capture0)")).append(";");
        out.newLine();
        emitExceptionCheck();
        out.dedent();
        out.emitLine("}");
        return;
    }

    emitCLine(out,
              2,
              "jmethodID _mj2x_indy_invoke_mid = (*env)->GetMethodID(env, "
              "_mj2x_indy_method_handle_cls, \"invokeWithArguments\", "
              "\"([Ljava/lang/Object;)Ljava/lang/Object;\");");
    out.appendLineStart("jobject _mj2x_indy_result = ").append(trackLocalRefExpr(
        "(*env)->CallObjectMethod(env, _mj2x_indy_target, "
        "_mj2x_indy_invoke_mid, _mj2x_indy_args)")).append(";");
    out.newLine();
    emitExceptionCheck();

    if (retType == PrimitiveKind::VOID) {
        out.emitLine("(void)_mj2x_indy_result;");
    }
    else if (retType == PrimitiveKind::OBJECT) {
        out.emitLine("stack[++sp].l = _mj2x_indy_result;");
    }
    else {
        out.appendLineStart("jclass _mj2x_indy_ret_box_cls = (jclass)").append(trackLocalRefExpr(
            "(*env)->FindClass(env, \"" + std::string(boxedClassName(retType)) + "\")")).append(";");
        out.newLine();
        emitCLine(out,
                  2,
                  "jmethodID _mj2x_indy_unbox_mid = (*env)->GetMethodID(env, "
                  "_mj2x_indy_ret_box_cls, " +
                      cStringLiteral(boxedUnboxMethod(retType)) + ", " +
                      cStringLiteral(boxedUnboxDesc(retType)) + ");");
        out.appendLineStart("stack[++sp]").append(stackValueAccessor(retType)).append(" = (")
            .append(narrowOnOperandStack(retType) ? "jint" : primitiveToJNIType(retType))
            .append(")(*env)->Call").append(primitiveToJNISuffix(retType))
            .append("Method(env, _mj2x_indy_result, _mj2x_indy_unbox_mid);");
        out.newLine();
    }
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitUnsupported(const char* what, codegen::CSourceStream& out)
{
    out.appendLineStart("jclass ex_class = (jclass)").append(
        trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/UnsupportedOperationException\")")).append(";");
    out.newLine();
    out.appendLineStart("(*env)->ThrowNew(env, ex_class, \"").append(what).append("\");");
    out.newLine();
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
}

}   // namespace mj2x

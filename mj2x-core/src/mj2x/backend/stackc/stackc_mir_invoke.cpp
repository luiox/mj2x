/// @file stackc_mir_invoke.cpp
/// @brief StackC 方法调用 / invokedynamic 发射器（从 stackc_backend.cpp 拆出）。
///
/// @details 缩进约定：调用时 out 处于方法体层（1 层），emitInvokeDynamic 内部
///          主体块 indent 到 2 层，嵌套块用 indent()/dedent() 管理。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

#include "mj2x/backend/indy_bootstrap_plan.hpp"
#include "libca/str/format.hpp"

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
    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，
    // 落点参数按必选口径走函数级局部 arena（结果仅瞬时喂给 jvmDescToPrimitive）。
    ca::str::Utf8StringArena retScratch;
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor(retScratch));
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

    // 用户自定义 intrinsic（issue #818）：匹配调用点降级为直接 symbol 调用，
    // 参数从 operand stack 弹出（与 JNI 调用同一参数区），结果写回接收者槽，
    // sp 调整与普通调用一致；mayThrow 时保留 pending 异常检查。
    // 匹配契约：static 调用点匹配 passReceiver=false 条目；instance 调用点
    // （receiver 在 sp-argCount 槽）仅匹配显式 passReceiver=true 条目
    // （fail-closed：未声明不匹配，回通用 JNI 派发——G-1 行为保持）。
    // static 调用点 + passReceiver 条目的配置校验错误在 compileMethod 入口
    // 预检 fail-closed，此处不会遇到该组合。
    {
        const UserIntrinsic* userIntr = nullptr;
        for (const auto& u : userIntrinsics) {
            if (u.owner == owner && u.name == name && u.desc == desc &&
                u.passReceiver != isStatic) {
                userIntr = &u;
                break;
            }
        }
        if (userIntr != nullptr) {
            // JVM 语义保持：instance 调用 receiver 为 NULL 时抛 NPE（与通用
            // JNI 派发路径一致），不把 NULL 静默递给用户实现。
            if (!isStatic) {
                emitNullPointerTrap("stack[" + recvExpr + "].l", out);
            }
            std::string call = userIntr->symbol + "(";
            bool        first = true;
            if (userIntr->passContext) {
                call += "env";
                first = false;
            }
            if (userIntr->passReceiver) {
                if (!first) call += ", ";
                first = false;
                // receiver 是参数区之下第一槽的裸 jobject（局部引用直传）。
                call += "stack[" + recvExpr + "].l";
            }
            for (int i = 0; i < argCount; ++i) {
                if (!first) call += ", ";
                first = false;
                const auto pka = jvmSortToPrimitive(argTypes[static_cast<size_t>(i)].getSort());
                call += "stack[sp - " + std::to_string(argCount - 1 - i) + "]" +
                        std::string(stackValueAccessor(pka));
            }
            call += ")";
            if (retType != PrimitiveKind::VOID) {
                if (argCount == 0) {
                    out.appendLineStart("stack[++sp]").append(stackValueAccessor(retType));
                }
                else {
                    out.appendLineStart("stack[sp - ").appendInt(resultIndex).append("]")
                        .append(stackValueAccessor(retType));
                }
                out.append(" = ").append(call).append(";").newLine();
            }
            else {
                out.appendLineStart(call).append(";").newLine();
            }
            if (isStatic) {
                if (retType == PrimitiveKind::VOID) {
                    if (argCount > 0) {
                        out.appendLineStart("sp -= ").appendInt(argCount).append(";").newLine();
                    }
                }
                else if (argCount > 0) {
                    out.appendLineStart("sp -= ").appendInt(argCount - 1).append(";").newLine();
                }
            }
            else {
                int popCount = argCount + 1;
                if (retType != PrimitiveKind::VOID) popCount -= 1;
                if (popCount > 0) {
                    out.appendLineStart("sp -= ").appendInt(popCount).append(";").newLine();
                }
            }
            if (userIntr->mayThrow) emitPendingExceptionCheck(out);
            return;
        }
    }

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
    // #921 staticArena 退役：同 emitInvoke，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena     retScratch;
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor(retScratch));
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
        if (stringsEncrypt) {
            // --strings-encrypt：indy 参数中的字符串常量改走加密 UTF-16 池
            // （JNI_OnLoad 批量解密），不内联明文 NewStringUTF 字面量。
            const auto offset = stringPoolOffsetFor(value);
            const auto length = stringPoolLengthFor(value);
            out.appendLineStart("jstring ").append(varName).append(" = (jstring)").append(
                trackLocalRefExpr(ca::str::format_std(
                    "(*env)->NewString(env, (const jchar*)(_mj2x_string_pool + {}), {})",
                    offset, length))).append(";");
            out.newLine();
            return;
        }
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
        // #1027 B3：tag→find* 形态单源（原 StackC/RegC 两份同式 switch 收敛到
        // indyHandleFindFor；未知 tag → findVirtual 默认形态）。
        const IndyHandleFind find = indyHandleFindFor(tag);
        if (find.argShape == IndyHandleArgShape::NameFieldType) {
            emitStringObject(varName + "_name", name);
            emitClassObjectForDescriptor(varName + "_field_type", desc);
        }
        else {
            if (find.emitName) {
                emitStringObject(varName + "_name", name);
            }
            emitMethodType(varName + "_type", desc);
        }

        std::string args = varName + "_owner_cls";
        switch (find.argShape) {
        case IndyHandleArgShape::NameFieldType:
            args += ", " + varName + "_name, " + varName + "_field_type";
            break;
        case IndyHandleArgShape::NameType:
            args += ", " + varName + "_name, " + varName + "_type";
            break;
        case IndyHandleArgShape::NameTypeOwner:
            args += ", " + varName + "_name, " + varName + "_type, _mj2x_owner_class";
            break;
        case IndyHandleArgShape::TypeOnly:
            args += ", " + varName + "_type";
            break;
        }
        emitCLine(out,
                  3,
                  "jmethodID " + varName + "_find_mid = (*env)->GetMethodID(env, " + varName +
                      "_lookup_cls, " + cStringLiteral(find.findName) + ", " +
                      cStringLiteral(find.findDesc) + ");");
        out.appendLineStart(varName).append(" = ").append(
            trackLocalRefExpr("(*env)->CallObjectMethod(env, _mj2x_indy_lookup, " + varName +
                              "_find_mid, " + args + ")")).append(";");
        out.newLine();
        emitExceptionCheck();
        out.dedent();
        out.emitLine("}");
    };

    auto emitAnyAsObject = [&](const std::string& varName, const ca::core::Any& value) {
        // #1027 B3：分类判定单源 = classifyIndyBsmArg（原 if 链公共判定序收敛）。
        switch (classifyIndyBsmArg(value)) {
        case IndyBsmArgKind::String:
            if (auto* p = value.as<std::string>()) {
                emitStringObject(varName, *p);
            }
            else if (auto* p = value.as<ca::str::Utf8StringRef>()) {
                emitStringObject(varName, p->to_std_string());
            }
            break;
        case IndyBsmArgKind::MethodType:
        case IndyBsmArgKind::Class:
            if (auto* p = value.as<Type>()) {
                if (p->getSort() == Type::METHOD) {
                    emitMethodType(varName, methodTypeDescriptorString(*p));
                }
                else {
                    emitClassObjectForDescriptor(varName, typeDescriptorString(*p));
                }
            }
            break;
        case IndyBsmArgKind::Handle:
            if (auto* p = value.as<Handle>()) {
                emitMethodHandle(varName, *p);
            }
            break;
        case IndyBsmArgKind::Int:
            if (auto* p = value.as<ca::i32>()) {
                emitBoxPrimitiveExpr(varName, PrimitiveKind::INT, "(jint)" + std::to_string(*p));
            }
            break;
        case IndyBsmArgKind::Long:
            if (auto* p = value.as<ca::i64>()) {
                emitBoxPrimitiveExpr(
                    varName, PrimitiveKind::LONG, "(jlong)" + std::to_string(*p) + "LL");
            }
            break;
        case IndyBsmArgKind::Float:
            if (auto* p = value.as<float>()) {
                emitBoxPrimitiveExpr(varName, PrimitiveKind::FLOAT, formatFloatLiteral(*p, 9) + "f");
            }
            break;
        case IndyBsmArgKind::Double:
            if (auto* p = value.as<double>()) {
                emitBoxPrimitiveExpr(varName, PrimitiveKind::DOUBLE, formatFloatLiteral(*p, 17));
            }
            break;
        case IndyBsmArgKind::Null:
            out.appendLineStart("jobject ").append(varName).append(" = NULL;");
            out.newLine();
            break;
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

    const bool useOneObjectInvokeHelper =
        retType == PrimitiveKind::OBJECT && argCount == 1 &&
        (argTypes[0].getSort() == Type::OBJECT || argTypes[0].getSort() == Type::ARRAY);

    out.emitLine("{");
    out.indent();
    // java/lang/Object 的 jclass 按 call-site 静态缓存（#879 X3）：稳态每次调用
    // 原来都重发 FindClass + 局部引用跟踪，而该类是 boot 类、结果恒一。缓存为
    // 全局引用后稳态只剩一次静态读；冷路径保持原形态并回填缓存。
    // 缓存键与 indyTargetCache 同口径（含类/方法/站点定位），生命周期 = native
    // library，泄漏上界 = indy 站点数。
    // #879 X3 评估结论：setup 块不随快/慢分流下沉。稳态成本已是静态读 + NULL
    // 分支（零 JNI）；若移入慢路径，快路径站点的 bootstrap 块（物理上在快路径
    // 调用点之前执行、且由快慢共用发射）里的 bsm varargs 数组须自行物化
    // jclass——多一套发射形态、多一处作用域耦合，换不来稳态 JNI 调用的减少。
    const auto indyObjectClsCache =
        cacheIdentifier("indy_object_cls",
                        currentClassName + "." + currentMethodName + currentMethodDesc + ":" +
                            node->name.to_std_string() + ":" + node->desc.to_std_string());
    out.appendLineStart("static jclass ").append(indyObjectClsCache).append(" = NULL;");
    out.newLine();
    out.emitLine("jclass _mj2x_indy_object_cls = " + indyObjectClsCache + ";");
    out.emitLine("if (_mj2x_indy_object_cls == NULL) {");
    out.indent();
    out.appendLineStart("_mj2x_indy_object_cls = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/Object\")")).append(";");
    out.newLine();
    out.appendLineStart("if (_mj2x_indy_object_cls != NULL) " + indyObjectClsCache +
                        " = (*env)->NewGlobalRef(env, _mj2x_indy_object_cls);");
    out.newLine();
    out.dedent();
    out.emitLine("}");
    // 快路径（OBJECT 返回 + 单对象参数）：参数在原栈槽捕获、原位覆写结果，
    // 不建 varargs 数组（省每次调用一次 NewObjectArray + SetObjectArrayElement）。
    // 一般路径：参数装箱进 Object[] 再弹栈。args 数组声明只随慢路径发射
    //（#879 X3 残余：快路径生成代码不再出现 _mj2x_indy_args，连 NULL 声明
    // 也不留——发射期快/慢尾二选一，作用域无跨分支引用）。
    if (useOneObjectInvokeHelper) {
        out.emitLine("jobject _mj2x_indy_capture0 = stack[sp].l;");
    }
    else {
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
    // #1027 B3：槽位判定单源 = planIndyBsmSlots（三后端原游走逐分支同构）。
    // 形参 Type 仍取自 bsmArgTypes——StaticArg 物化需要原语宽度（发射面契约）。
    const auto bsmSlots = planIndyBsmSlots(bsmDesc, node->bsmArgs.size());
    for (size_t i = 0; i < bsmSlots.size(); ++i) {
        const auto& slot = bsmSlots[i];
        switch (slot.kind) {
        case IndyBsmSlotKind::Lookup:
            bsmExprs.push_back("_mj2x_indy_lookup");
            break;
        case IndyBsmSlotKind::SiteName:
            bsmExprs.push_back("_mj2x_indy_name");
            break;
        case IndyBsmSlotKind::SiteType:
            bsmExprs.push_back("_mj2x_indy_type");
            break;
        case IndyBsmSlotKind::Varargs: {
            const size_t remaining = slot.varargCount;
            out.appendLineStart("jobjectArray _mj2x_indy_bsm_varargs = (jobjectArray)").append(
                trackLocalRefExpr("(*env)->NewObjectArray(env, " + std::to_string(remaining) +
                                  ", _mj2x_indy_object_cls, NULL)")).append(";");
            out.newLine();
            for (size_t j = 0; j < remaining; ++j) {
                const std::string argVar = "_mj2x_indy_bsm_vararg_" + std::to_string(j);
                emitAnyAsObject(argVar, node->bsmArgs[slot.bsmArgIndex + j]);
                out.appendLineStart("(*env)->SetObjectArrayElement(env, _mj2x_indy_bsm_varargs, ")
                    .appendInt(static_cast<int64_t>(j)).append(", ").append(argVar).append(");");
                out.newLine();
            }
            bsmExprs.push_back("_mj2x_indy_bsm_varargs");
            break;
        }
        case IndyBsmSlotKind::Null:
            bsmExprs.push_back("NULL");
            break;
        case IndyBsmSlotKind::StaticArg: {
            const auto&     formal = bsmArgTypes[i];   // 槽 ↔ 形参一一对应
            const std::string argVar =
                "_mj2x_indy_bsm_arg_" + std::to_string(slot.bsmArgIndex);
            bsmExprs.push_back(emitAnyForFormal(argVar, formal, node->bsmArgs[slot.bsmArgIndex]));
            break;
        }
        }
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
    if (useOneObjectInvokeHelper) {
        out.appendLineStart("jclass _mj2x_indy_cloader_cls = (jclass)").append(trackLocalRefExpr(
            "(*env)->FindClass(env, \"mj2x/cloader/Loader\")")).append(";");
        out.newLine();
        emitCLine(out,
                  2,
                  "jmethodID _mj2x_indy_helper_mid = (*env)->GetStaticMethodID(env, "
                  "_mj2x_indy_cloader_cls, \"invokeMethodHandle1\", "
                  "\"(Ljava/lang/invoke/MethodHandle;Ljava/lang/Object;)Ljava/lang/Object;\");");
        // 参数在块首已捕获（pop 之前），结果原位覆写参数槽——sp 净效应 0。
        out.appendLineStart("stack[sp].l = ").append(trackLocalRefExpr(
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

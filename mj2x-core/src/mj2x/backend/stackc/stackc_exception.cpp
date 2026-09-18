/// @file stackc_exception.cpp
/// @brief StackC 异常路径发射器：除零/NPE trap、pending 异常检查、throw/monitor。
///
/// @details 所有发射器约定：调用时 out 处于方法体层（1 层 = 4 空格），内部
///          嵌套块用 CSourceStream::indent()/dedent() 管理，行级文本一律走
///          emitLine/appendLineStart，不出现手写空格拼接。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

namespace mj2x {

void StackCBackend::Impl::emitIntegerDivZeroTrap(const char* rhsAccessor, codegen::CSourceStream& out)
{
    out.appendLineStart("if (stack[sp]").append(rhsAccessor).append(" == 0) {");
    out.newLine();
    out.indent();
    out.appendLineStart("jclass ex_class = (jclass)").append(trackLocalRefExpr(
        "(*env)->FindClass(env, \"java/lang/ArithmeticException\")")).append(";");
    out.newLine();
    out.emitLine("(*env)->ThrowNew(env, ex_class, \"/ by zero\");");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitNullPointerTrap(const std::string& refExpr, codegen::CSourceStream& out)
{
    codegen::CSourceStream dispatch;
    dispatch.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        dispatch.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, dispatch);
    }
    out.emitMultiline(renderStackCSnippet(
        StackCSnippet::NullPointerTrap,
        {{"ref", refExpr},
         {"ex_class",
          trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/NullPointerException\")")},
         {"dispatch", dispatch.str()}}));
}

void StackCBackend::Impl::emitPendingExceptionCheck(codegen::CSourceStream& out)
{
    // lite 模式：无异常 handler 时省略检查——JNI 调用失败设置的 pending 异常会在
    // native 方法返回时由 JVM 自动传播（对标 JNIC 稀疏检查）。有 handler 的
    // dispatch 边界必须保留（否则异常无法路由到 catch 块）。
    if (exceptionCheckLite && currentExceptionDispatchTarget.empty()) {
        return;
    }
    // dispatch 在 if 块内（相对 1 层）；模板去首层缩进后由 emitMultiline 按 out.indent
    // 平移整段，调用点处于任意缩进层都正确（旧式用 ${pad} 注入绝对空格已移除）。
    codegen::CSourceStream dispatch;
    dispatch.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        dispatch.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, dispatch);
    }
    out.emitMultiline(renderStackCSnippet(StackCSnippet::PendingExceptionCheck,
                                         {{"dispatch", dispatch.str()}}));
}

void StackCBackend::Impl::emitMonitor(const MIRMonitor* node, codegen::CSourceStream& out)
{
    // JVM 语义（JVMS monitorenter/monitorexit）：objectref 为 NULL 时抛
    // NullPointerException。JNI MonitorEnter/MonitorExit 对 NULL jobject 的行为
    // 未定义（HotSpot resolve_non_null 直接崩溃），必须先做 NPE trap——与
    // RegC（发射面守卫）/ VMC4（monitor intrinsic helper）/ LLVM
    // （mj2x_llvm_monitor_*）的 null → NPE 守卫对齐。
    emitNullPointerTrap("stack[sp].l", out);
    if (node->enter) {
        out.emitLine("(*env)->MonitorEnter(env, stack[sp--].l);");
    }
    else {
        out.emitLine("(*env)->MonitorExit(env, stack[sp--].l);");
    }
}

void StackCBackend::Impl::emitThrow(const MIRThrow* node, codegen::CSourceStream& out)
{
    (void)node;
    // JVM 语义（JVMS athrow）：objectref 为 NULL 时抛 NullPointerException。
    // JNI Throw 对 NULL throwable 的行为未定义（HotSpot 按"调用方保证非空"处理），
    // 必须先做 NPE trap——与 RegC（_mj2x_regc_throw）/ VMC4（mj2x_vm_throw）/
    // LLVM（mj2x_llvm_throw）的 null → NPE 守卫对齐。
    emitNullPointerTrap("stack[sp].l", out);
    out.emitLine("(*env)->Throw(env, stack[sp--].l);");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
}

}   // namespace mj2x

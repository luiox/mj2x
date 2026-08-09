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
    for (int i = 0; i < 2; ++i) dispatch.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        dispatch.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, dispatch);
    }
    out.append(renderStackCSnippet(
        StackCSnippet::NullPointerTrap,
        {{"ref", refExpr},
         {"ex_class",
          trackLocalRefExpr("(*env)->FindClass(env, \"java/lang/NullPointerException\")")},
         {"dispatch", dispatch.str()}}));
}

void StackCBackend::Impl::emitPendingExceptionCheck(codegen::CSourceStream& out, int indent)
{
    // indent 是模板内嵌的绝对空格数（8）；dispatch 分支内容在模板内再进一级（12 空格 = 3 层）。
    const int          level = indent / 4;
    const std::string  pad   = std::string(static_cast<size_t>(indent), ' ');
    codegen::CSourceStream dispatch;
    for (int i = 0; i < level + 1; ++i) dispatch.indent();
    if (!currentExceptionDispatchTarget.empty()) {
        dispatch.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, dispatch);
    }
    out.append(renderStackCSnippet(StackCSnippet::PendingExceptionCheck,
                                   {{"pad", pad}, {"dispatch", dispatch.str()}}));
}

void StackCBackend::Impl::emitMonitor(const MIRMonitor* node, codegen::CSourceStream& out)
{
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
    out.emitLine("(*env)->Throw(env, stack[sp--].l);");
    if (!currentExceptionDispatchTarget.empty()) {
        out.emitLine("goto " + currentExceptionDispatchTarget + ";");
    }
    else {
        emitExceptionReturn(currentMethodDesc, out);
    }
}

}   // namespace mj2x

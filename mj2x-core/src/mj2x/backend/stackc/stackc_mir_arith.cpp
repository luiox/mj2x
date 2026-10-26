/// @file stackc_mir_arith.cpp
/// @brief StackC 算术/常量/局部/转换/比较/分支发射器（从 stackc_backend.cpp 拆出）。
///
/// @details 缩进约定：调用时 out 处于方法体层（1 层 = 4 空格）；常量/局部/
///          算术类发射器输出单行语句（emitLine 跟随当前层），snippet 模板
///          自带完整缩进文本经 append 整体写入。
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"

#include "mj2x/core/jvm_type.hpp"

#include "libca/str/format.hpp"
#include <cmath>

namespace mj2x {

using jbytecode::core::Label;
using namespace jbytecode::analysis;
using codegen::cFloatLiteral;
using codegen::cStringLiteral;
using codegen::fnv1a64;
using codegen::formatFloatLiteral;
using codegen::hex64;

void StackCBackend::Impl::emitConstant(const MIRConstant* node, codegen::CSourceStream& out)
{
    const char* acc = stackAccessor(node->type);
    if (std::holds_alternative<int64_t>(node->value)) {
        auto v = std::get<int64_t>(node->value);
        if (node->type == PrimitiveKind::LONG) {
            // 用 unsigned 64-bit 字面量再转 jlong，避免 INT64_MIN 被解析成越界有符号字面量。
            const auto u = static_cast<uint64_t>(v);
            out.emitLine(ca::str::format_std("stack[++sp]{} = (jlong){}ULL;",
                                            acc, static_cast<int64_t>(u)));
        }
        else {
            // int：先转 uint32 再转 jint，保证 INT_MIN 等值无符号环绕语义、字面量不越界。
            const auto u = static_cast<uint32_t>(static_cast<int32_t>(v));
            out.emitLine(ca::str::format_std("stack[++sp]{} = (jint){}u;",
                                            acc, static_cast<int64_t>(u)));
        }
    }
    else if (std::holds_alternative<double>(node->value)) {
        const auto d = std::get<double>(node->value);
        if (node->type == PrimitiveKind::FLOAT) {
            // float 字面量加 f 后缀；NaN/Inf 走宏保证可移植（MSVC/gcc 均支持）。
            std::string value;
            if (std::isnan(d))
                value = "NAN";
            else if (std::isinf(d))
                value = "(" + std::string(d < 0 ? "-" : "") + "INFINITY)";
            else
                value = cFloatLiteral(d, 9) + "f";
            out.emitLine(ca::str::format_std("stack[++sp]{} = {};", acc, value));
        }
        else {
            std::string value;
            if (std::isnan(d))
                value = "(double)NAN";
            else if (std::isinf(d))
                value = "(" + std::string(d < 0 ? "-" : "") + "(double)INFINITY)";
            else
                value = cFloatLiteral(d, 17);
            out.emitLine(ca::str::format_std("stack[++sp]{} = {};", acc, value));
        }
    }
    else if (std::holds_alternative<ca::str::Utf8StringRef>(node->value)) {
        // UTF-16 code units 池（防 strings 扫描），等价 JNIC 3.5.1/3.7.0
        // stringObf=false 形态：NewString(env, units, len)，无明文 UTF-8 字节。
        // #1117 LDC 恒等：JVM ldc 常量是 interned 实例——首次物化经
        // String.intern() 归一化进 JVM 字符串池，缓存在 TU 级
        // _mj2x_literal_cache[]（global ref）；同内容常量跨站点/跨方法/
        // 跨 String.intern() 恒等，命中路径 0 JNI、不耗 local ref。
        const auto value  = std::get<ca::str::Utf8StringRef>(node->value).to_std_string();
        const auto offset = stringPoolOffsetFor(value);
        const auto length = stringPoolLengthFor(value);
        const auto slot   = stringPoolSlotFor(value);
        out.emitLine("{");
        out.indent();
        out.emitLine(ca::str::format_std("if (_mj2x_literal_cache[{}] == NULL) {{", slot));
        out.indent();
        out.emitLine(ca::str::format_std(
            "jstring lit_fresh = (jstring)(*env)->NewString(env, (const jchar*)"
            "(_mj2x_string_pool + {}), {});",
            offset, length));
        out.emitLine("static jmethodID intern_mid = NULL;");
        out.emitLine("if (intern_mid == NULL) {");
        out.indent();
        out.emitLine("jclass intern_class = (*env)->FindClass(env, \"java/lang/String\");");
        out.emitLine("if (intern_class != NULL) {");
        out.indent();
        out.emitLine("intern_mid = (*env)->GetMethodID(env, intern_class, \"intern\", "
                     "\"()Ljava/lang/String;\");");
        out.emitLine("(*env)->DeleteLocalRef(env, intern_class);");
        out.dedent();
        out.emitLine("}");
        out.dedent();
        out.emitLine("}");
        out.emitLine("jstring lit_pooled = intern_mid != NULL ? (jstring)(*env)->"
                     "CallObjectMethod(env, lit_fresh, intern_mid) : NULL;");
        out.emitLine("if (lit_pooled == NULL) lit_pooled = lit_fresh; "
                     "else (*env)->DeleteLocalRef(env, lit_fresh);");
        out.emitLine(ca::str::format_std(
            "_mj2x_literal_cache[{}] = (*env)->NewGlobalRef(env, lit_pooled);", slot));
        out.dedent();
        out.emitLine("}");
        out.emitLine(
            ca::str::format_std("stack[++sp]{} = _mj2x_literal_cache[{}];", acc, slot));
        out.dedent();
        out.emitLine("}");
    }
    else if (std::holds_alternative<Type>(node->value)) {
        const auto type = std::get<Type>(node->value);
        if (type.getSort() == Type::METHOD) {
            out.appendLineStart("stack[++sp]").append(acc).append(" = NULL;");
            out.newLine();
            return;
        }
        if (type.getSort() == Type::OBJECT || type.getSort() == Type::ARRAY ||
            type.getSort() == Type::INTERNAL) {
            // class literal：走类引用缓存（JNIC cached_c_N 形态），避免循环内重复
            // find_class 反射链；self 类直接取 _mj2x_owner_class。块包裹保证
            // 多个 class literal 相邻时局部变量/static 缓存名不冲突。
            out.emitLine("{");
            out.indent();
            // #921 staticArena 退役：描述符仅瞬时转串成缓存键，函数级局部 arena 零驻留。
            ca::str::Utf8StringArena scratch;
            emitClassRefCache(type.getDescriptor(scratch).to_std_string(), out);
            out.appendLineStart("stack[++sp].l = lc;");
            out.newLine();
            out.dedent();
            out.emitLine("}");
            return;
        }
        const auto kind = jvmSortToPrimitive(type.getSort());
        out.emitLine("{");
        out.indent();
        out.appendLineStart("jclass wrapper = (jclass)").append(trackLocalRefExpr(
            "(*env)->FindClass(env, \"" + std::string(boxedClassName(kind)) + "\")")).append(";");
        out.newLine();
        out.emitLine("jfieldID type_fid = (*env)->GetStaticFieldID(env, wrapper, \"TYPE\", "
                     "\"Ljava/lang/Class;\");");
        out.appendLineStart("stack[++sp].l = ")
            .append(trackLocalRefExpr("(*env)->GetStaticObjectField(env, wrapper, type_fid)")).append(";");
        out.newLine();
        out.dedent();
        out.emitLine("}");
    }
    else {
        out.appendLineStart("stack[++sp]").append(acc).append(" = NULL;");
        out.newLine();
    }
}

void StackCBackend::Impl::emitLocalLoad(const MIRLocalLoad* node, codegen::CSourceStream& out)
{
    out.emitMultiline(renderStackCSnippet(
        StackCSnippet::LocalLoad,
        {{"acc", stackAccessor(node->type)}, {"slot", std::to_string(node->varIndex)}}));
}

void StackCBackend::Impl::emitLocalStore(const MIRLocalStore* node, codegen::CSourceStream& out)
{
    out.emitMultiline(renderStackCSnippet(
        StackCSnippet::LocalStore,
        {{"acc", stackAccessor(node->type)}, {"slot", std::to_string(node->varIndex)}}));
}

void StackCBackend::Impl::emitIinc(const MIRIinc* node, codegen::CSourceStream& out)
{
    out.appendLineStart("locals[").appendInt(node->varIndex).append("].i += ")
        .appendInt(node->increment).append(";");
    out.newLine();
}

void StackCBackend::Impl::emitBinaryOp(const MIRBinaryOp* node, codegen::CSourceStream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* lhs = "stack[sp - 1]";
    const char* rhs = "stack[sp]";

    // JVM 整数算术是确定的有符号二补码环绕；C int 溢出是 UB。
    // 对 ADD/SUB/MUL/SHL/NEG/USHR 显式走无符号运算再转回，保证环绕语义。
    // DIV/REM：除零必须抛 ArithmeticException；MIN_VALUE / -1 和 % -1 也显式处理，
    // 避免 C 有符号除法 UB。
    if (node->type == PrimitiveKind::INT) {
        switch (node->op) {
        case BinaryOp::ADD:
        case BinaryOp::SUB:
        case BinaryOp::MUL:
            out.emitMultiline(renderStackCSnippet(StackCSnippet::BinaryUnsignedWrap,
                                           {{"lhs", lhs},
                                            {"rhs", rhs},
                                            {"acc", acc},
                                            {"signed_type", "jint"},
                                            {"unsigned_type", "uint32_t"},
                                            {"op", binaryOpToC(node->op)}}));
            return;
        case BinaryOp::SHL:
            out.appendLineStart(lhs).append(acc)
                .append(" = (jint)((uint32_t)").append(lhs).append(acc)
                .append(" << (").append(rhs).append(acc).append(" & 31));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::SHR:
            out.appendLineStart(lhs).append(acc)
                .append(" = (jint)(").append(lhs).append(acc)
                .append(" >> (").append(rhs).append(acc).append(" & 31));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::USHR:
            // 无符号右移：先把操作数转 uint32 再 >> 。
            out.appendLineStart(lhs).append(acc)
                .append(" = (jint)((uint32_t)").append(lhs).append(acc)
                .append(" >> (").append(rhs).append(acc).append(" & 31));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::DIV:
            emitIntegerDivZeroTrap(acc, out);
            out.appendLineStart("if (").append(lhs).append(acc)
                .append(" == (jint)0x80000000u && ").append(rhs).append(acc).append(" == -1) {");
            out.newLine();
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = (jint)0x80000000u;");
            out.newLine();
            out.dedent();
            out.emitLine("} else {");
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = ").append(lhs).append(acc)
                .append(" / ").append(rhs).append(acc).append(";");
            out.newLine();
            out.dedent();
            out.emitLine("}");
            out.emitLine("sp--;");
            return;
        case BinaryOp::REM:
            emitIntegerDivZeroTrap(acc, out);
            out.appendLineStart("if (").append(lhs).append(acc)
                .append(" == (jint)0x80000000u && ").append(rhs).append(acc).append(" == -1) {");
            out.newLine();
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = 0;");
            out.newLine();
            out.dedent();
            out.emitLine("} else {");
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = ").append(lhs).append(acc)
                .append(" % ").append(rhs).append(acc).append(";");
            out.newLine();
            out.dedent();
            out.emitLine("}");
            out.emitLine("sp--;");
            return;
        default: break;
        }
    }
    if (node->type == PrimitiveKind::LONG) {
        switch (node->op) {
        case BinaryOp::ADD:
        case BinaryOp::SUB:
        case BinaryOp::MUL:
            out.emitMultiline(renderStackCSnippet(StackCSnippet::BinaryUnsignedWrap,
                                           {{"lhs", lhs},
                                            {"rhs", rhs},
                                            {"acc", acc},
                                            {"signed_type", "jlong"},
                                            {"unsigned_type", "uint64_t"},
                                            {"op", binaryOpToC(node->op)}}));
            return;
        case BinaryOp::SHL:
            out.appendLineStart(lhs).append(acc)
                .append(" = (jlong)((uint64_t)").append(lhs).append(acc)
                .append(" << (").append(rhs).append(".i & 63));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::SHR:
            out.appendLineStart(lhs).append(acc)
                .append(" = (jlong)(").append(lhs).append(acc)
                .append(" >> (").append(rhs).append(".i & 63));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::USHR:
            out.appendLineStart(lhs).append(acc)
                .append(" = (jlong)((uint64_t)").append(lhs).append(acc)
                .append(" >> (").append(rhs).append(".i & 63));");
            out.newLine();
            out.emitLine("sp--;");
            return;
        case BinaryOp::DIV:
            emitIntegerDivZeroTrap(acc, out);
            out.appendLineStart("if (").append(lhs).append(acc)
                .append(" == (jlong)0x8000000000000000ULL && ").append(rhs).append(acc)
                .append(" == -1) {");
            out.newLine();
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = (jlong)0x8000000000000000ULL;");
            out.newLine();
            out.dedent();
            out.emitLine("} else {");
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = ").append(lhs).append(acc)
                .append(" / ").append(rhs).append(acc).append(";");
            out.newLine();
            out.dedent();
            out.emitLine("}");
            out.emitLine("sp--;");
            return;
        case BinaryOp::REM:
            emitIntegerDivZeroTrap(acc, out);
            out.appendLineStart("if (").append(lhs).append(acc)
                .append(" == (jlong)0x8000000000000000ULL && ").append(rhs).append(acc)
                .append(" == -1) {");
            out.newLine();
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = 0;");
            out.newLine();
            out.dedent();
            out.emitLine("} else {");
            out.indent();
            out.appendLineStart(lhs).append(acc).append(" = ").append(lhs).append(acc)
                .append(" % ").append(rhs).append(acc).append(";");
            out.newLine();
            out.dedent();
            out.emitLine("}");
            out.emitLine("sp--;");
            return;
        default: break;
        }
    }
    if (node->type == PrimitiveKind::FLOAT && node->op == BinaryOp::REM) {
        out.appendLineStart(lhs).append(acc).append(" = fmodf(").append(lhs).append(acc)
            .append(", ").append(rhs).append(acc).append(");");
        out.newLine();
        out.emitLine("sp--;");
        return;
    }
    if (node->type == PrimitiveKind::DOUBLE && node->op == BinaryOp::REM) {
        out.appendLineStart(lhs).append(acc).append(" = fmod(").append(lhs).append(acc)
            .append(", ").append(rhs).append(acc).append(");");
        out.newLine();
        out.emitLine("sp--;");
        return;
    }
    // float/double 或 DIV/REM/SHR/AND/OR/XOR：直接发原生运算符。
    out.appendLineStart(lhs).append(acc).append(" = ").append(lhs).append(acc).append(" ")
        .append(binaryOpToC(node->op)).append(" ").append(rhs).append(acc).append(";");
    out.newLine();
    out.emitLine("sp--;");
}

void StackCBackend::Impl::emitCmp(const MIRCmp* node, codegen::CSourceStream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* lhs = "stack[sp - 1]";
    const char* rhs = "stack[sp]";
    if (node->type == PrimitiveKind::FLOAT || node->type == PrimitiveKind::DOUBLE) {
        out.appendLineStart("if (isnan((double)").append(lhs).append(acc)
            .append(") || isnan((double)").append(rhs).append(acc).append(")) {");
        out.newLine();
        out.indent();
        out.appendLineStart(lhs).append(".i = ").append(node->nanGreater ? "1" : "-1").append(";");
        out.newLine();
        out.dedent();
        out.emitLine("} else {");
        out.indent();
        out.appendLineStart(lhs).append(".i = (").append(lhs).append(acc).append(" > ")
            .append(rhs).append(acc).append(") - (").append(lhs).append(acc).append(" < ")
            .append(rhs).append(acc).append(");");
        out.newLine();
        out.dedent();
        out.emitLine("}");
    }
    else {
        out.appendLineStart(lhs).append(".i = (").append(lhs).append(acc).append(" > ")
            .append(rhs).append(acc).append(") - (").append(lhs).append(acc).append(" < ")
            .append(rhs).append(acc).append(");");
        out.newLine();
    }
    out.emitLine("sp--;");
}

void StackCBackend::Impl::emitCast(const MIRCast* node, codegen::CSourceStream& out)
{
    const char* fromAcc = stackAccessor(node->from);
    const char* toAcc   = castResultAccessor(node->to);
    if (node->from == PrimitiveKind::FLOAT || node->from == PrimitiveKind::DOUBLE) {
        if (node->to == PrimitiveKind::INT) {
            out.emitLine("{");
            out.indent();
            out.appendLineStart("jdouble v = (jdouble)stack[sp]").append(fromAcc).append(";");
            out.newLine();
            out.emitLine("if (isnan(v)) stack[sp].i = 0;");
            out.emitLine("else if (v > 2147483647.0) stack[sp].i = 2147483647;");
            out.emitLine("else if (v < -2147483648.0) stack[sp].i = (jint)2147483648u;");
            out.emitLine("else stack[sp].i = (jint)v;");
            out.dedent();
            out.emitLine("}");
            return;
        }
        if (node->to == PrimitiveKind::LONG) {
            out.emitLine("{");
            out.indent();
            out.appendLineStart("jdouble v = (jdouble)stack[sp]").append(fromAcc).append(";");
            out.newLine();
            out.emitLine("if (isnan(v)) stack[sp].j = 0;");
            out.emitLine("else if (v >= 9223372036854775808.0) stack[sp].j = "
                         "(jlong)0x7fffffffffffffffLL;");
            out.emitLine("else if (v <= -9223372036854775808.0) stack[sp].j = "
                         "(jlong)0x8000000000000000ULL;");
            out.emitLine("else stack[sp].j = (jlong)v;");
            out.dedent();
            out.emitLine("}");
            return;
        }
    }
    if (node->to == PrimitiveKind::BYTE) {
        out.appendLineStart("stack[sp].i = (jint)(jbyte)stack[sp]").append(fromAcc).append(";");
        out.newLine();
    }
    else if (node->to == PrimitiveKind::CHAR) {
        out.appendLineStart("stack[sp].i = (jint)(jchar)stack[sp]").append(fromAcc).append(";");
        out.newLine();
    }
    else if (node->to == PrimitiveKind::SHORT) {
        out.appendLineStart("stack[sp].i = (jint)(jshort)stack[sp]").append(fromAcc).append(";");
        out.newLine();
    }
    else {
        out.appendLineStart("stack[sp]").append(toAcc).append(" = (")
            .append(primitiveToJNIType(node->to)).append(")stack[sp]").append(fromAcc).append(";");
        out.newLine();
    }
}

void StackCBackend::Impl::emitUnaryOp(const MIRUnaryOp* node, codegen::CSourceStream& out)
{
    const char* acc = stackAccessor(node->type);
    const char* tgt = "stack[sp]";
    if (node->type == PrimitiveKind::INT) {
        // ~x + 1 = -x（二补码环绕，避免一元负号在 INT_MIN 上的 UB）。
        out.appendLineStart(tgt).append(acc).append(" = (jint)(~(uint32_t)").append(tgt)
            .append(acc).append(" + 1u);");
        out.newLine();
    }
    else if (node->type == PrimitiveKind::LONG) {
        out.appendLineStart(tgt).append(acc).append(" = (jlong)(~(uint64_t)").append(tgt)
            .append(acc).append(" + 1ULL);");
        out.newLine();
    }
    else {
        out.appendLineStart(tgt).append(acc).append(" = -").append(tgt).append(acc).append(";");
        out.newLine();
    }
}

void StackCBackend::Impl::emitBranch(const MIRBranch* node, codegen::CSourceStream& out)
{
    const char* acc = stackAccessor(node->type);
    out.emitLine("{");
    out.indent();
    if (node->type == PrimitiveKind::OBJECT) {
        // if_acmpeq/acmpne：JNI 引用是句柄，同一对象的 local ref 与 global ref
        // 句柄指针值不同，裸 == 会漏判（#1117 g17.string_intern 实测：ldc 字面量
        // 缓存持 global ref，String.intern() 返回 local ref，恒不等）。与 RegC
        // 同口径走 IsSameObject 判定对象同一性（对 NULL 也正确：两侧 NULL 为真）。
        // 字节码面 acmp 只有 eq/ne 两种谓词。
        const bool negate = (node->op == CmpOp::NE);
        out.appendLineStart("int cond = ").append(negate ? "!" : "")
            .append("(*env)->IsSameObject(env, stack[sp - 1].l, stack[sp].l);");
    }
    else {
        out.appendLineStart("int cond = (stack[sp - 1]").append(acc).append(" ")
            .append(cmpOpToC(node->op)).append(" stack[sp]").append(acc).append(");");
    }
    out.newLine();
    out.emitLine("sp -= 2;");
    out.appendLineStart("if (cond) goto ")
        .append(labelIdent(labelIndex.at(node->targetLabel))).append(";");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitBranchZero(const MIRBranchZero* node, codegen::CSourceStream& out)
{
    const char* acc  = stackAccessor(node->type);
    const char* zero = (node->type == PrimitiveKind::OBJECT) ? "NULL" : "0";
    out.emitLine("{");
    out.indent();
    out.appendLineStart("int cond = (stack[sp]").append(acc).append(" ")
        .append(cmpOpToC(node->op)).append(" ").append(zero).append(");");
    out.newLine();
    out.emitLine("sp--;");
    out.appendLineStart("if (cond) goto ")
        .append(labelIdent(labelIndex.at(node->targetLabel))).append(";");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitUncondBranch(const MIRUncondBranch* node, codegen::CSourceStream& out)
{
    out.appendLineStart("goto ").append(labelIdent(labelIndex.at(node->targetLabel))).append(";");
    out.newLine();
}

void StackCBackend::Impl::emitSwitch(const MIRSwitch* node, codegen::CSourceStream& out)
{
    out.emitLine("switch (stack[sp--].i) {");
    out.indent();
    for (auto& c : node->cases) {
        out.appendLineStart("case ").appendInt(c.key).append(": goto ")
            .append(labelIdent(labelIndex.at(c.targetLabel))).append(";");
        out.newLine();
    }
    out.appendLineStart("default: goto ")
        .append(labelIdent(labelIndex.at(node->defaultLabel))).append(";");
    out.newLine();
    out.dedent();
    out.emitLine("}");
}

void StackCBackend::Impl::emitReturnValue(const MIRReturnValue* node, codegen::CSourceStream& out)
{
    if (node->type == PrimitiveKind::OBJECT) {
        out.emitLine("{");
        out.indent();
        out.emitLine("jobject _mj2x_ret = stack[sp--].l;");
        out.emitLine("return _mj2x_ret;");
        out.dedent();
        out.emitLine("}");
        return;
    }
    out.emitLine("{");
    out.indent();
    out.appendLineStart(primitiveToJNIType(node->type)).append(" _mj2x_ret = stack[sp--]")
        .append(stackAccessor(node->type)).append(";");
    out.newLine();
    out.emitLine("return _mj2x_ret;");
    out.dedent();
    out.emitLine("}");
}

}   // namespace mj2x

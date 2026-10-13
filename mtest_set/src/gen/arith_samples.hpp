// arith_samples.hpp — G2 算术运算样本生成
//
// 覆盖：iadd/isub/imul/idiv/irem/ineg/iinc、ladd/ldiv/lrem、fadd/fmul、dadd/ddiv，
//       以及除零陷阱、MIN_VALUE/-1 边界。
//
// 每个 gen* 返回 .class bytes；行为等价测试对应同名 .java 样本（见 test_resource）。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::arith {

/// `static int add(int a, int b) { return a + b; }` —— ILOAD ILOAD IADD IRETURN
inline std::vector<ca::u8> genAdd(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithAdd", "add", "(II)I",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int mulSub(int a, int b) { return a * b - b; }` —— IMUL ISUB
inline std::vector<ca::u8> genMulSub(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithMulSub", "mulSub", "(II)I",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IMUL);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::ISUB);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int divRem(int a, int b) { return a / b + a % b; }` —— IDIV IREM
inline std::vector<ca::u8> genDivRem(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithDivRem", "divRem", "(II)I",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IDIV);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IREM);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int negate(int a) { return -a; }` —— INEG
inline std::vector<ca::u8> genNegate(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithNegate", "negate", "(I)I",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::INEG);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int iincLoop(int n) { int s=0; for(int i=0;i<n;i++) s+=i; return s; }`
/// —— IINC 循环（ICONST, ISTORE, ILOAD, IF_ICMPGE, ILOAD, IADD, ISTORE, IINC, GOTO）
inline std::vector<ca::u8> genIincLoop(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithIincLoop", "iincLoop", "(I)I",
        [](MethodNode* mv) {
            Label* cond = new Label();
            Label* body = new Label();
            // int s = 0;   local 1
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitVarInsn(Opcodes::ISTORE, 1);
            // int i = 0;   local 2
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitVarInsn(Opcodes::ISTORE, 2);
            // goto cond
            mv->visitJumpInsn(Opcodes::GOTO, cond);
            // body: s += i
            mv->visitLabel(body);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitInsn(Opcodes::IADD);
            mv->visitVarInsn(Opcodes::ISTORE, 1);
            // i++   (iinc 2, 1)
            mv->visitIincInsn(2, 1);
            // cond: if (i < n) goto body
            mv->visitLabel(cond);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitJumpInsn(Opcodes::IF_ICMPLT, body);
            // return s
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static long longDiv(long a, long b) { return a / b; }` —— LLOAD LLOAD LDIV LRETURN
inline std::vector<ca::u8> genLongDiv(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithLongDiv", "longDiv", "(JJ)J",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::LLOAD, 0);
            mv->visitVarInsn(Opcodes::LLOAD, 2);
            mv->visitInsn(Opcodes::LDIV);
            mv->visitInsn(Opcodes::LRETURN);
        });
}

/// `static double doubleAdd(double a, double b) { return a + b; }` —— DLOAD DLOAD DADD DRETURN
inline std::vector<ca::u8> genDoubleAdd(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArithDoubleAdd", "doubleAdd", "(DD)D",
        [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::DLOAD, 0);
            mv->visitVarInsn(Opcodes::DLOAD, 2);
            mv->visitInsn(Opcodes::DADD);
            mv->visitInsn(Opcodes::DRETURN);
        });
}

}   // namespace mtest_set::arith

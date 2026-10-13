// array_samples.hpp — G8 数组样本生成
//
// 覆盖：newarray/anewarray、iaload/iastore、aaload/aastore、arraylength。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::arrays {

/// `static int sumArray(int[] a) { int s=0; for(int i=0;i<a.length;i++) s+=a[i]; return s; }`
/// —— NEWARRAY(不，参数传入)、ARRAYLENGTH、IALOAD、IADD、循环
/// 实际：参数是 int[]，用 ARRAYLENGTH + IALOAD
inline std::vector<ca::u8> genSumArray(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArrSumArray", "sumArray", "([I)I",
        [](MethodNode* mv) {
            Label* cond = new Label();
            Label* body = new Label();
            // int s = 0;  local 1
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitVarInsn(Opcodes::ISTORE, 1);
            // int i = 0;  local 2
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitVarInsn(Opcodes::ISTORE, 2);
            mv->visitJumpInsn(Opcodes::GOTO, cond);
            // body: s += a[i]
            mv->visitLabel(body);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitVarInsn(Opcodes::ALOAD, 0);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitInsn(Opcodes::IALOAD);
            mv->visitInsn(Opcodes::IADD);
            mv->visitVarInsn(Opcodes::ISTORE, 1);
            mv->visitIincInsn(2, 1);
            // cond: if i < a.length goto body
            mv->visitLabel(cond);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitVarInsn(Opcodes::ALOAD, 0);
            mv->visitInsn(Opcodes::ARRAYLENGTH);
            mv->visitJumpInsn(Opcodes::IF_ICMPLT, body);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int[] makeAndFill(int n)` —— NEWARRAY + IASTORE，返回填满 n 的数组
inline std::vector<ca::u8> genMakeAndFill(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ArrMakeAndFill", "makeAndFill", "(I)[I",
        [](MethodNode* mv) {
            Label* cond = new Label();
            Label* body = new Label();
            // int[] a = new int[n];  local 1
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitIntInsn(Opcodes::NEWARRAY, Opcodes::T_INT);
            mv->visitVarInsn(Opcodes::ASTORE, 1);
            // int i = 0;  local 2
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitVarInsn(Opcodes::ISTORE, 2);
            mv->visitJumpInsn(Opcodes::GOTO, cond);
            // body: a[i] = n
            mv->visitLabel(body);
            mv->visitVarInsn(Opcodes::ALOAD, 1);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::IASTORE);
            mv->visitIincInsn(2, 1);
            // cond: if i < a.length goto body
            mv->visitLabel(cond);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitVarInsn(Opcodes::ALOAD, 1);
            mv->visitInsn(Opcodes::ARRAYLENGTH);
            mv->visitJumpInsn(Opcodes::IF_ICMPLT, body);
            mv->visitVarInsn(Opcodes::ALOAD, 1);
            mv->visitInsn(Opcodes::ARETURN);
        });
}

}   // namespace mtest_set::arrays

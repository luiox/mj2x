// const_load_samples.hpp — G1 常量加载样本生成
//
// 覆盖：iconst_*、bipush、sipush、ldc（int/String）、aconst_null、lconst、fconst、dconst。
// 通过一个 `()Ljava/lang/String;` 方法返回不同常量来验证 LDC 字符串，
// 以及 `()I` 方法返回各种整型常量。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::const_load {

/// `static int constInts()` —— ICONST_M1..ICONST_5 + BIPUSH + SIPUSH，求和后返回。
///  (-1)+0+1+2+3+4+5 + 100 + 5000 = 5114
inline std::vector<ca::u8> genConstInts(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ConstInts", "constInts", "()I",
        [](MethodNode* mv) {
            mv->visitInsn(Opcodes::ICONST_M1);
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::ICONST_1);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::ICONST_2);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::ICONST_3);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::ICONST_4);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::ICONST_5);
            mv->visitInsn(Opcodes::IADD);
            mv->visitIntInsn(Opcodes::BIPUSH, 100);
            mv->visitInsn(Opcodes::IADD);
            mv->visitIntInsn(Opcodes::SIPUSH, 5000);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static String ldcString()` —— LDC 字符串常量
inline std::vector<ca::u8> genLdcString(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ConstLdcString", "ldcString",
        "()Ljava/lang/String;",
        [](MethodNode* mv) {
            mv->visitLdcInsn(std::string("mtest_set ldc string"));
            mv->visitInsn(Opcodes::ARETURN);
        });
}

/// `static long lconst()` —— LCONST_0 + LCONST_1 + LADD
inline std::vector<ca::u8> genLconst(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ConstLconst", "lconst", "()J",
        [](MethodNode* mv) {
            mv->visitInsn(Opcodes::LCONST_0);
            mv->visitInsn(Opcodes::LCONST_1);
            mv->visitInsn(Opcodes::LADD);
            mv->visitInsn(Opcodes::LRETURN);
        });
}

/// `static int aconstNull()` —— ACONST_NULL + IFNULL 分支返回 0/1
inline std::vector<ca::u8> genAconstNull(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "ConstAconstNull", "aconstNull", "()I",
        [](MethodNode* mv) {
            Label* isNull = new Label();
            mv->visitInsn(Opcodes::ACONST_NULL);
            mv->visitJumpInsn(Opcodes::IFNULL, isNull);
            mv->visitInsn(Opcodes::ICONST_1);
            mv->visitInsn(Opcodes::IRETURN);
            mv->visitLabel(isNull);
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

}   // namespace mtest_set::const_load

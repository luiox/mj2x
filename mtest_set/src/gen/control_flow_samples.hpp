// control_flow_samples.hpp — G5 控制流样本生成
//
// 覆盖：if_icmp* 分支、GOTO、tableswitch、lookupswitch、ifnull/ifnonnull。
// 这些样本验证 COMPUTE_FRAMES 对分支栈帧的正确计算。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::control_flow {

/// `static int abs(int a) { return a < 0 ? -a : a; }` —— IF_ICMPGE 分支 + INEG
inline std::vector<ca::u8> genAbs(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "CfAbs", "abs", "(I)I",
        [](MethodNode* mv) {
            Label* elseBranch = new Label();
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::ICONST_0);
            mv->visitJumpInsn(Opcodes::IF_ICMPGE, elseBranch);   // a >= 0 时进入非负分支
            // a < 0: return -a
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::INEG);
            mv->visitInsn(Opcodes::IRETURN);
            // else: return a
            mv->visitLabel(elseBranch);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int denseSwitch(int k) { switch(k){case 0:return 100;case 1:return 200;...} }`
/// —— TABLESWITCH（密集）
inline std::vector<ca::u8> genDenseSwitch(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "CfDenseSwitch", "denseSwitch", "(I)I",
        [](MethodNode* mv) {
            Label* dflt   = new Label();
            Label* case0  = new Label();
            Label* case1  = new Label();
            Label* case2  = new Label();
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitTableSwitchInsn(0, 2, dflt, {case0, case1, case2});
            // case 0
            mv->visitLabel(case0);
            mv->visitIntInsn(Opcodes::SIPUSH, 100);
            mv->visitInsn(Opcodes::IRETURN);
            // case 1
            mv->visitLabel(case1);
            mv->visitIntInsn(Opcodes::SIPUSH, 200);
            mv->visitInsn(Opcodes::IRETURN);
            // case 2
            mv->visitLabel(case2);
            mv->visitIntInsn(Opcodes::SIPUSH, 300);
            mv->visitInsn(Opcodes::IRETURN);
            // default
            mv->visitLabel(dflt);
            mv->visitInsn(Opcodes::ICONST_M1);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static int sparseSwitch(int k)` —— LOOKUPSWITCH（稀疏）
inline std::vector<ca::u8> genSparseSwitch(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "CfSparseSwitch", "sparseSwitch", "(I)I",
        [](MethodNode* mv) {
            Label* dflt = new Label();
            Label* c10  = new Label();
            Label* c20  = new Label();
            Label* c30  = new Label();
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitLookupSwitchInsn(dflt, {10, 20, 30}, {c10, c20, c30});
            mv->visitLabel(c10);
            mv->visitIntInsn(Opcodes::SIPUSH, 100);
            mv->visitInsn(Opcodes::IRETURN);
            mv->visitLabel(c20);
            mv->visitIntInsn(Opcodes::SIPUSH, 200);
            mv->visitInsn(Opcodes::IRETURN);
            mv->visitLabel(c30);
            mv->visitIntInsn(Opcodes::SIPUSH, 300);
            mv->visitInsn(Opcodes::IRETURN);
            mv->visitLabel(dflt);
            mv->visitInsn(Opcodes::ICONST_M1);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

/// `static String nullCheck(Object o) { return o != null ? "y" : "n"; }`
/// —— IFNONNULL/IFNULL + String LDC + ARETURN
inline std::vector<ca::u8> genNullCheck(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(arena, "CfNullCheck", "nullCheck",
        "(Ljava/lang/Object;)Ljava/lang/String;",
        [](MethodNode* mv) {
            Label* nonNull = new Label();
            mv->visitVarInsn(Opcodes::ALOAD, 0);
            mv->visitJumpInsn(Opcodes::IFNONNULL, nonNull);
            mv->visitLdcInsn(std::string("n"));
            mv->visitInsn(Opcodes::ARETURN);
            mv->visitLabel(nonNull);
            mv->visitLdcInsn(std::string("y"));
            mv->visitInsn(Opcodes::ARETURN);
        });
}

}   // namespace mtest_set::control_flow

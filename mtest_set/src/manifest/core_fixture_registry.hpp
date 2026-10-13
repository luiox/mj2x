// core_fixture_registry.hpp — mtest_set 样本的「中性事实」单一事实源
//
// 背景：pass_fixture / native_fixture / semantic_fixture 三份 manifest 服务不同
// 消费方（mbase 优化 pass / mj2x native 转换 / mtest_set 自测），但对同一个样本
// id 都要复述同样的行为契约：调用片段（runnerBody）、期望 stdout（expectedStdout）、
// 该样本必须出现的 opcode。以前这些字面量在三份 manifest 里各写一遍，样本改一次
// 输出要同步改三处，是明确的漂移陷阱。
//
// 本文件把这批「与消费方无关」的事实收敛到唯一一处，按 id 建索引。各领域 manifest
// 退化成「按 id 装饰核心事实 + 补自己的领域字段（PassTarget / backends / generator）」
// 的视图，不再重抄 runnerBody / expectedStdout。
//
// 只放中性事实：
//   - mainClass      Driver 里调用的入口类（含包名，如 "calls.CallInterface"）
//   - runnerBody     Driver.main 中调用被测方法并打印的代码片段
//   - expectedStdout 期望 stdout（不含末尾换行，由调用方补）
//   - opcodes        该样本必须出现的 opcode（覆盖矩阵单一事实源）
// generator / generatedClass 路径 / PassTarget / backends 是领域字段，留在各 manifest。

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "jbytecode/core/opcodes.hpp"

namespace mtest_set {

/// 一个样本与消费方无关的中性事实。跨三份 manifest 共享，按 id 唯一。
struct CoreFixture
{
    const char*      id;
    const char*      mainClass;
    const char*      runnerBody;
    const char*      expectedStdout;
    std::vector<int> opcodes;
};

/// 全部中性事实。id 唯一；新增样本先在此登记，再在领域 manifest 里引用。
inline const std::vector<CoreFixture>& coreFixtureRegistry()
{
    using jbytecode::core::Opcodes;
    static const std::vector<CoreFixture> registry = {
        // ---- G1 常量加载 ----
        {"g1.const_ints", "ConstInts",
         "System.out.println(\"constInts=\" + ConstInts.constInts());",
         "constInts=5114",
         {Opcodes::IADD, Opcodes::IRETURN, Opcodes::BIPUSH, Opcodes::SIPUSH}},
        {"g1.ldc_string", "ConstLdcString",
         "System.out.println(\"ldcString=\" + ConstLdcString.ldcString());",
         "ldcString=mtest_set ldc string",
         {Opcodes::LDC, Opcodes::ARETURN}},
        {"g1.lconst", "ConstLconst",
         "System.out.println(\"lconst=\" + ConstLconst.lconst());",
         "lconst=1",
         {Opcodes::LCONST_0, Opcodes::LCONST_1, Opcodes::LADD, Opcodes::LRETURN}},
        {"g1.aconst_null", "ConstAconstNull",
         "System.out.println(\"aconstNull=\" + ConstAconstNull.aconstNull());",
         "aconstNull=0",
         {Opcodes::ACONST_NULL, Opcodes::IFNULL}},
        // ---- G2 算术 ----
        {"g2.add", "ArithAdd",
         "System.out.println(\"add(3,4)=\" + ArithAdd.add(3, 4));",
         "add(3,4)=7",
         {Opcodes::IADD}},
        {"g2.mul_sub", "ArithMulSub",
         "System.out.println(\"mulSub(5,2)=\" + ArithMulSub.mulSub(5, 2));",
         "mulSub(5,2)=8",
         {Opcodes::IMUL, Opcodes::ISUB}},
        {"g2.div_rem", "ArithDivRem",
         "System.out.println(\"divRem(17,5)=\" + ArithDivRem.divRem(17, 5));",
         "divRem(17,5)=5",
         {Opcodes::IDIV, Opcodes::IREM}},
        {"g2.negate", "ArithNegate",
         "System.out.println(\"negate(7)=\" + ArithNegate.negate(7));",
         "negate(7)=-7",
         {Opcodes::INEG}},
        {"g2.iinc_loop", "ArithIincLoop",
         "System.out.println(\"iincLoop(10)=\" + ArithIincLoop.iincLoop(10));",
         "iincLoop(10)=45",
         {Opcodes::IINC, Opcodes::GOTO, Opcodes::IF_ICMPLT}},
        {"g2.long_div", "ArithLongDiv",
         "System.out.println(\"longDiv(100L,7L)=\" + ArithLongDiv.longDiv(100L, 7L));",
         "longDiv(100L,7L)=14",
         {Opcodes::LDIV, Opcodes::LLOAD, Opcodes::LRETURN}},
        {"g2.double_add", "ArithDoubleAdd",
         "System.out.println(\"doubleAdd(1.5,2.5)=\" + ArithDoubleAdd.doubleAdd(1.5, 2.5));",
         "doubleAdd(1.5,2.5)=4.0",
         {Opcodes::DADD, Opcodes::DLOAD, Opcodes::DRETURN}},
        // ---- G5 控制流 ----
        {"g5.abs", "CfAbs",
         "System.out.println(\"abs(-5)=\" + CfAbs.abs(-5));",
         "abs(-5)=5",
         {Opcodes::IF_ICMPGE, Opcodes::INEG}},
        {"g5.dense_switch", "CfDenseSwitch",
         "System.out.println(\"denseSwitch(1)=\" + CfDenseSwitch.denseSwitch(1));",
         "denseSwitch(1)=200",
         {Opcodes::TABLESWITCH, Opcodes::SIPUSH}},
        {"g5.sparse_switch", "CfSparseSwitch",
         "System.out.println(\"sparseSwitch(20)=\" + CfSparseSwitch.sparseSwitch(20));",
         "sparseSwitch(20)=200",
         {Opcodes::LOOKUPSWITCH, Opcodes::SIPUSH}},
        {"g5.null_check", "CfNullCheck",
         "System.out.println(\"nullCheck(null)=\" + CfNullCheck.nullCheck(null));",
         "nullCheck(null)=n",
         {Opcodes::IFNONNULL, Opcodes::LDC, Opcodes::ARETURN}},
        // ---- G8 数组 ----
        {"g8.sum_array", "ArrSumArray",
         "System.out.println(\"sumArray=\" + ArrSumArray.sumArray(new int[]{1, 2, 3, 4, 5}));",
         "sumArray=15",
         {Opcodes::ARRAYLENGTH, Opcodes::IALOAD}},
        {"g8.make_and_fill", "ArrMakeAndFill",
         "System.out.print(\"makeAndFill(4)=\"); "
         "for (int value : ArrMakeAndFill.makeAndFill(4)) System.out.print(value + \" \"); "
         "System.out.println();",
         "makeAndFill(4)=4 4 4 4 ",
         {Opcodes::NEWARRAY, Opcodes::IASTORE, Opcodes::ARETURN}},
        // ---- G3 类型转换（semantic Wave1）----
        {"g3.integral_narrowing", "TypeIntegralNarrowing",
         "System.out.println(\"narrow=\" + TypeIntegralNarrowing.run(0x12345678));",
         "narrow=44392",
         {Opcodes::I2B, Opcodes::I2S, Opcodes::I2C}},
        {"g3.numeric_widening", "TypeNumericWidening",
         "System.out.println(\"widen=\" + TypeNumericWidening.run(3));",
         "widen=12.0",
         {Opcodes::I2L, Opcodes::I2F, Opcodes::I2D, Opcodes::L2F, Opcodes::L2D, Opcodes::F2D}},
        {"g3.numeric_narrowing", "TypeNumericNarrowing",
         "System.out.println(\"narrowFloat=\" + TypeNumericNarrowing.run(7.5));",
         "narrowFloat=28",
         {Opcodes::D2I, Opcodes::D2L, Opcodes::D2F, Opcodes::F2I, Opcodes::F2L, Opcodes::L2I}},
        // ---- G6 方法调用（semantic Wave1）----
        {"g6.static_special", "CallsStaticSpecial",
         "System.out.println(\"calls=\" + CallsStaticSpecial.run(5));",
         "calls=24",
         {Opcodes::INVOKESTATIC, Opcodes::INVOKESPECIAL, Opcodes::INVOKEVIRTUAL}},
        {"g6.virtual_interface", "calls.CallInterface",
         "System.out.println(\"iface=\" + calls.CallInterface.run(7));",
         "iface=14",
         {Opcodes::INVOKESPECIAL, Opcodes::INVOKEINTERFACE}},
        {"g6.recursion", "CallsRecursion",
         "System.out.println(\"recursion=\" + CallsRecursion.sum(6));",
         "recursion=21",
         {Opcodes::INVOKESTATIC, Opcodes::IFLE}},
        // ---- G7 字段访问（semantic Wave1）----
        {"g7.static_fields", "FieldsStatic",
         "System.out.println(\"staticField=\" + FieldsStatic.run(4));",
         "staticField=7",
         {Opcodes::GETSTATIC, Opcodes::PUTSTATIC}},
        {"g7.instance_fields", "FieldsInstance",
         "System.out.println(\"instanceField=\" + FieldsInstance.run(4));",
         "instanceField=5",
         {Opcodes::GETFIELD, Opcodes::PUTFIELD, Opcodes::INVOKESPECIAL}},
        // ---- G9 对象/类型（semantic Wave2）----
        {"g9.object_lifecycle", "ObjectLifecycle",
         "System.out.println(\"object=\" + ObjectLifecycle.run(9));",
         "object=9",
         {Opcodes::NEW, Opcodes::DUP, Opcodes::INVOKESPECIAL, Opcodes::INVOKEVIRTUAL}},
        {"g9.type_checks", "TypeChecks",
         "System.out.println(\"types=\" + TypeChecks.run(\"hello\") + \":\" + TypeChecks.run(Integer.valueOf(1)));",
         "types=5:-1",
         {Opcodes::INSTANCEOF, Opcodes::CHECKCAST}},
        // ---- G10 异常/monitor（semantic Wave2）----
        {"g10.throw_catch", "ExceptionFlow",
         "System.out.println(\"exceptions=\" + ExceptionFlow.run(0) + \":\" + ExceptionFlow.run(4));",
         "exceptions=-7:25",
         {Opcodes::ATHROW, Opcodes::IDIV}},
        {"g10.monitor", "MonitorFlow",
         "System.out.println(\"monitor=\" + MonitorFlow.run(new Object()));",
         "monitor=7",
         {Opcodes::MONITORENTER, Opcodes::MONITOREXIT, Opcodes::ATHROW}},
        // ---- G11 反射（semantic Wave2）----
        {"g11.reflection_access", "reflect.ReflectionAccess",
         "System.out.println(\"reflect=\" + reflect.ReflectionAccess.className() + \":\" + reflect.ReflectionAccess.run());",
         "reflect=reflect.ReflectionTarget:17",
         {Opcodes::INVOKESTATIC, Opcodes::INVOKEVIRTUAL, Opcodes::ANEWARRAY,
          Opcodes::AASTORE, Opcodes::CHECKCAST}},
        // ---- G12-G15（semantic Wave3，#820：吸收 bludnov/ttvar 语义覆盖面）----
        {"g12.string_ops", "StringOps",
         "System.out.println(\"stringOps=\" + StringOps.run(\"hello\"));",
         "stringOps=604",
         {Opcodes::INVOKEVIRTUAL, Opcodes::IMUL, Opcodes::IADD}},
        {"g12.string_compare", "StringCompare",
         "System.out.println(\"eqSame=\" + StringCompare.eqSame());\n"
         "System.out.println(\"cmpLess=\" + StringCompare.cmpLess());\n"
         "System.out.println(\"cmpGreater=\" + StringCompare.cmpGreater());\n"
         "System.out.println(\"eqIgnoreCase=\" + StringCompare.eqIgnoreCase());",
         "eqSame=true\n"
         "cmpLess=-1\n"
         "cmpGreater=1\n"
         "eqIgnoreCase=true",
         {Opcodes::LDC, Opcodes::INVOKEVIRTUAL, Opcodes::IRETURN}},
        {"g13.bit_ops", "BitOps",
         "System.out.println(\"bitOps=\" + BitOps.run(0x5A5A5A5A));",
         "bitOps=-9",
         {Opcodes::ISHL, Opcodes::IUSHR, Opcodes::IAND, Opcodes::IXOR, Opcodes::IOR}},
        {"g13.shift_out_of_range", "ShiftOutOfRange",
         "System.out.println(\"iShl=\" + ShiftOutOfRange.iShl(33, -1));\n"
         "System.out.println(\"iShr=\" + ShiftOutOfRange.iShr(32, 33));\n"
         "System.out.println(\"iUShr=\" + ShiftOutOfRange.iUShr(32, 33));\n"
         "System.out.println(\"lShl=\" + ShiftOutOfRange.lShl(64, 65));\n"
         "System.out.println(\"lUShr=\" + ShiftOutOfRange.lUShr(64, 65));",
         "iShl=-2147483606\n"
         "iShr=103\n"
         "iUShr=-2147483647\n"
         "lShl=-9223372036854775805\n"
         "lUShr=-9223372036854775807",
         {Opcodes::ISHL, Opcodes::ISHR, Opcodes::IUSHR, Opcodes::LSHL, Opcodes::LUSHR,
          Opcodes::IXOR, Opcodes::LXOR}},
        {"g14.deep_recursion", "DeepRecursion",
         "System.out.println(\"fib(8)=\" + DeepRecursion.run(8));",
         "fib(8)=21",
         {Opcodes::IF_ICMPLE, Opcodes::INVOKESTATIC, Opcodes::ISUB, Opcodes::IADD}},
        {"g15.collection_ops", "CollectionOps",
         "java.util.List l = new java.util.ArrayList(); l.add(1); l.add(2); l.add(3);\n"
         "System.out.println(\"collectionSize=\" + CollectionOps.run(l));",
         "collectionSize=3",
         {Opcodes::INVOKEINTERFACE, Opcodes::IRETURN}},
        // ---- G16 static final 常量折叠靶（#960）----
        {"g16.cv_constant", "SvfCvConstant",
         "System.out.println(\"cvConstant=\" + SvfCvConstant.readCv());",
         "cvConstant=42",
         {Opcodes::GETSTATIC, Opcodes::IRETURN}},
        {"g16.clinit_constant", "SvfClinitConstant",
         "System.out.println(\"clinitConstant=\" + SvfClinitConstant.readDelayed());",
         "clinitConstant=5",
         {Opcodes::GETSTATIC, Opcodes::PUTSTATIC, Opcodes::IRETURN}},
        // ---- G17 jvmtp 1.7.1 正确性修复语义点回归（#1117）----
        {"g17.math_nan_zero", "MathNanZero",
         "System.out.println(\"maxDNanL=\" + MathNanZero.maxDNanL());\n"
         "System.out.println(\"maxDNanR=\" + MathNanZero.maxDNanR());\n"
         "System.out.println(\"minDNanL=\" + MathNanZero.minDNanL());\n"
         "System.out.println(\"minDNanR=\" + MathNanZero.minDNanR());\n"
         "System.out.println(\"maxFNanL=\" + MathNanZero.maxFNanL());\n"
         "System.out.println(\"maxFNanR=\" + MathNanZero.maxFNanR());\n"
         "System.out.println(\"minFNanL=\" + MathNanZero.minFNanL());\n"
         "System.out.println(\"minFNanR=\" + MathNanZero.minFNanR());\n"
         "System.out.println(\"maxDZeroPN=\" + MathNanZero.maxDZeroPN());\n"
         "System.out.println(\"maxDZeroNP=\" + MathNanZero.maxDZeroNP());\n"
         "System.out.println(\"minDZeroPN=\" + MathNanZero.minDZeroPN());\n"
         "System.out.println(\"minDZeroNP=\" + MathNanZero.minDZeroNP());\n"
         "System.out.println(\"maxFZeroPN=\" + MathNanZero.maxFZeroPN());\n"
         "System.out.println(\"maxFZeroNP=\" + MathNanZero.maxFZeroNP());\n"
         "System.out.println(\"minFZeroPN=\" + MathNanZero.minFZeroPN());\n"
         "System.out.println(\"minFZeroNP=\" + MathNanZero.minFZeroNP());\n"
         "System.out.println(\"maxDNanBits=\" + MathNanZero.maxDNanBits());\n"
         "System.out.println(\"maxI=\" + MathNanZero.maxI());\n"
         "System.out.println(\"minI=\" + MathNanZero.minI());\n"
         "System.out.println(\"maxL=\" + MathNanZero.maxL());\n"
         "System.out.println(\"minL=\" + MathNanZero.minL());\n"
         "System.out.println(\"roundFHalfBelow=\" + MathNanZero.roundFHalfBelow());\n"
         "System.out.println(\"roundFOddBig=\" + MathNanZero.roundFOddBig());\n"
         "System.out.println(\"roundFNegOddBig=\" + MathNanZero.roundFNegOddBig());\n"
         "System.out.println(\"roundDHalfBelow=\" + MathNanZero.roundDHalfBelow());\n"
         "System.out.println(\"roundDOddBig=\" + MathNanZero.roundDOddBig());",
         "maxDNanL=true\n"
         "maxDNanR=true\n"
         "minDNanL=true\n"
         "minDNanR=true\n"
         "maxFNanL=true\n"
         "maxFNanR=true\n"
         "minFNanL=true\n"
         "minFNanR=true\n"
         "maxDZeroPN=0x0.0p0\n"
         "maxDZeroNP=0x0.0p0\n"
         "minDZeroPN=-0x0.0p0\n"
         "minDZeroNP=-0x0.0p0\n"
         "maxFZeroPN=0x0.0p0\n"
         "maxFZeroNP=0x0.0p0\n"
         "minFZeroPN=-0x0.0p0\n"
         "minFZeroNP=-0x0.0p0\n"
         "maxDNanBits=true\n"
         "maxI=7\n"
         "minI=-9\n"
         "maxL=1099511627776\n"
         "minL=-1\n"
         // round 边界（#1144）：HotSpot 17.0.20.1 实测；floor(x+0.5) 公式在
         // roundFHalfBelow 给 1、roundFOddBig 给 8388610、roundFNegOddBig 给
         // -8388610、roundDHalfBelow 给 1——发散形状即断言点。
         "roundFHalfBelow=0\n"
         "roundFOddBig=8388609\n"
         "roundFNegOddBig=-8388609\n"
         "roundDHalfBelow=0\n"
         "roundDOddBig=4503599627370497",
         {Opcodes::INVOKESTATIC, Opcodes::LDC, Opcodes::IRETURN}},
        {"g17.string_intern", "StringIntern",
         "System.out.println(\"ldcSame=\" + StringIntern.ldcSame());\n"
         "System.out.println(\"internVsLdc=\" + StringIntern.internVsLdc());\n"
         "System.out.println(\"newStringIntern=\" + StringIntern.newStringIntern());\n"
         "System.out.println(\"concatIntern=\" + StringIntern.concatIntern());",
         "ldcSame=true\n"
         "internVsLdc=true\n"
         "newStringIntern=true\n"
         "concatIntern=true",
         {Opcodes::LDC, Opcodes::IF_ACMPEQ, Opcodes::INVOKEVIRTUAL}},
        {"g17.array_store_check", "ArrayStoreCheck",
         "System.out.println(\"covariantStringAstoreThrows=\" + ArrayStoreCheck.covariantStringAstoreThrows());\n"
         "System.out.println(\"validCovariantStore=\" + ArrayStoreCheck.validCovariantStore());\n"
         "System.out.println(\"covariantLongIntoIntegerThrows=\" + ArrayStoreCheck.covariantLongIntoIntegerThrows());\n"
         "System.out.println(\"primitiveStoreOk=\" + ArrayStoreCheck.primitiveStoreOk());\n"
         "System.out.println(\"aseTypeName=\" + ArrayStoreCheck.aseTypeName());",
         "covariantStringAstoreThrows=true\n"
         "validCovariantStore=2\n"
         "covariantLongIntoIntegerThrows=true\n"
         "primitiveStoreOk=7\n"
         "aseTypeName=true",
         {Opcodes::ANEWARRAY, Opcodes::AASTORE, Opcodes::CHECKCAST}},
        {"g17.sibling_methods", "SiblingMethods",
         "System.out.println(\"s1=\" + SiblingMethods.s1(3));\n"
         "System.out.println(\"s2=\" + SiblingMethods.s2(5));\n"
         "System.out.println(\"s3=\" + SiblingMethods.s3(2.0f));\n"
         "System.out.println(\"s4=\" + SiblingMethods.s4(\"za\"));\n"
         "System.out.println(\"s5a=\" + SiblingMethods.s5(0));\n"
         "System.out.println(\"s5b=\" + SiblingMethods.s5(4));\n"
         "System.out.println(\"s6=\" + SiblingMethods.s6(4));\n"
         "System.out.println(\"s7=\" + SiblingMethods.s7());\n"
         "System.out.println(\"inst=\" + new SiblingMethods().inst(4));",
         "s1=29\n"
         "s2=5000022\n"
         "s3=3.25\n"
         "s4=-25\n"
         "s5a=-1\n"
         "s5b=25\n"
         "s6=14\n"
         "s7=sibling-shared\n"
         "inst=15",
         {Opcodes::INVOKEVIRTUAL, Opcodes::INVOKESPECIAL, Opcodes::NEWARRAY}},
        {"g17.annotation_visibility", "AnnotationProbe",
         "RetainedAnno a = AnnotationProbe.class.getMethod(\"annotated\").getAnnotation(RetainedAnno.class);\n"
         "System.out.println(\"annotated=\" + a.value() + \":\" + a.name());\n"
         "System.out.println(\"defaultValue=\" + RetainedAnno.class.getMethod(\"value\").getDefaultValue() + \":\" + RetainedAnno.class.getMethod(\"name\").getDefaultValue());\n"
         "System.out.println(\"plainNull=\" + (AnnotationProbe.class.getMethod(\"plain\").getAnnotation(RetainedAnno.class) == null));\n"
         "System.out.println(\"deprecated=\" + AnnotationProbe.class.getMethod(\"legacy\").isAnnotationPresent(Deprecated.class));",
         "annotated=7:m1\n"
         "defaultValue=42:probe\n"
         "plainNull=true\n"
         "deprecated=true",
         {Opcodes::BIPUSH, Opcodes::IRETURN, Opcodes::ICONST_1}},
    };
    return registry;
}

/// 按 id 查中性事实。找不到返回 nullptr（调用方 join 时应断言非空）。
inline const CoreFixture* findCoreFixture(const char* id)
{
    for (const auto& fixture : coreFixtureRegistry()) {
        if (std::string(fixture.id) == id) return &fixture;
    }
    return nullptr;
}

/// 按 id 查中性事实，找不到时打印 id 并终止进程。供各 manifest 的静态 join 使用：
/// join 发生在 static 初始化期，缺 id 若只靠 assert 兜底，NDEBUG 构建会直接空指针
/// 解引用崩溃且无任何线索；这里换成带消息的硬失败，Debug/Release 行为一致。
inline const CoreFixture& requireCoreFixture(const char* id, const char* consumer)
{
    const CoreFixture* core = findCoreFixture(id);
    if (!core) {
        std::fprintf(stderr, "[mtest_set] %s descriptor id \"%s\" 不在 core registry 中\n",
                     consumer, id);
        std::abort();
    }
    return *core;
}

}   // namespace mtest_set

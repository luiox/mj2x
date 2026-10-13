// native_fixture_manifest.hpp — mj2x native 转换（StackC/RegC）的样本 manifest
//
// 与 pass_fixture_manifest.hpp 并列：后者服务 mbase 的优化 pass 测试，本文件
// 服务 mj2x 的"Java 字节码 → native（.c → DLL）→ 行为等价"E2E 测试。
//
// 复用 mtest_set 现有 G1/G2/G5/G8 生成器（同一份 .class bytes）和 Java 源码样本
// （同一份行为基准），避免 mj2x 自行手写 inline Java 样本。
//
// 首版范围：仅列入 mj2x StackC 后端已确认能跑通的 case（纯计算/控制流/数组）。
// RegC 后端的支持度待 Step 4c 跑通后再逐步追加（RegC 已声明支持纯计算/分支/
// switch/数组，但需实测确认）。

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gen/arith_samples.hpp"
#include "gen/array_samples.hpp"
#include "gen/const_load_samples.hpp"
#include "gen/control_flow_samples.hpp"
#include "manifest/core_fixture_registry.hpp"

namespace mtest_set {

/// mj2x native 后端标识。
enum class NativeBackend
{
    StackC,
    RegC,
};

/// 单条 native 转换 fixture：把一个 mtest_set 生成样本交给指定后端做
/// Java→native 转换，期望运行行为与原 JVM 一致。
struct NativeFixtureCase
{
    const char*             id;                // 如 "g2.add"
    const char*             generatedClass;    // 如 "arith/ArithAdd.class"（相对 mtest_set/gen 输出根）
    const char*             javaSource;        // 相对 mtest_set 根的 .java 路径（行为基准）
    const char*             jvmRunnerBody;     // 来自 core registry
    const char*             expectedJvmOutput; // 来自 core registry
    /// 本 case 适用的后端。首版只含 StackC（RegC 待实测后追加）。
    std::vector<NativeBackend> backends;
    /// 生成器：用 libjbytecode 内存构造 .class bytes。
    std::vector<ca::u8> (*generator)(ca::str::Utf8StringArena&);
};

// native manifest 的领域描述：id + generatedClass/javaSource 路径 + backends + generator。
// jvmRunnerBody / expectedJvmOutput 从 core registry join，不在此重述。
struct NativeDescriptor
{
    const char*                id;
    const char*                generatedClass;
    const char*                javaSource;
    std::vector<NativeBackend> backends;
    std::vector<ca::u8> (*generator)(ca::str::Utf8StringArena&);
};

/// native 转换 fixture 清单。mj2x E2E 测试遍历此清单，对每条 case × backend
/// 跑"样本→mj2x→DLL→Java 调用对比"。
inline const std::vector<NativeFixtureCase>& nativeFixtureManifest()
{
    static const std::vector<NativeDescriptor> descriptors = {
        // ---- G1 常量加载 ----
        {"g1.const_ints", "const_load/ConstInts.class",
         "test_resource/java/const_load/ConstLoadSamples.java", {NativeBackend::StackC}, &const_load::genConstInts},
        {"g1.ldc_string", "const_load/ConstLdcString.class",
         "test_resource/java/const_load/ConstLoadSamples.java", {NativeBackend::StackC}, &const_load::genLdcString},
        {"g1.lconst", "const_load/ConstLconst.class",
         "test_resource/java/const_load/ConstLoadSamples.java", {NativeBackend::StackC}, &const_load::genLconst},
        {"g1.aconst_null", "const_load/ConstAconstNull.class",
         "test_resource/java/const_load/ConstLoadSamples.java", {NativeBackend::StackC}, &const_load::genAconstNull},
        // ---- G2 算术 ----
        {"g2.add", "arith/ArithAdd.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genAdd},
        {"g2.mul_sub", "arith/ArithMulSub.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genMulSub},
        {"g2.div_rem", "arith/ArithDivRem.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genDivRem},
        {"g2.negate", "arith/ArithNegate.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genNegate},
        {"g2.iinc_loop", "arith/ArithIincLoop.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genIincLoop},
        {"g2.long_div", "arith/ArithLongDiv.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genLongDiv},
        {"g2.double_add", "arith/ArithDoubleAdd.class",
         "test_resource/java/arith/ArithSamples.java", {NativeBackend::StackC}, &arith::genDoubleAdd},
        // ---- G5 控制流（NullCheck 涉及字符串，暂不列入）----
        {"g5.abs", "control_flow/CfAbs.class",
         "test_resource/java/control_flow/ControlFlowSamples.java", {NativeBackend::StackC}, &control_flow::genAbs},
        {"g5.dense_switch", "control_flow/CfDenseSwitch.class",
         "test_resource/java/control_flow/ControlFlowSamples.java", {NativeBackend::StackC}, &control_flow::genDenseSwitch},
        {"g5.sparse_switch", "control_flow/CfSparseSwitch.class",
         "test_resource/java/control_flow/ControlFlowSamples.java", {NativeBackend::StackC}, &control_flow::genSparseSwitch},
        // ---- G8 数组 ----
        {"g8.sum_array", "arrays/ArrSumArray.class",
         "test_resource/java/arrays/ArraySamples.java", {NativeBackend::StackC}, &arrays::genSumArray},
        {"g8.make_and_fill", "arrays/ArrMakeAndFill.class",
         "test_resource/java/arrays/ArraySamples.java", {NativeBackend::StackC}, &arrays::genMakeAndFill},
    };

    static const std::vector<NativeFixtureCase> manifest = [] {
        std::vector<NativeFixtureCase> cases;
        cases.reserve(descriptors.size());
        for (const auto& d : descriptors) {
            const CoreFixture* core = &requireCoreFixture(d.id, "native");
            cases.push_back(NativeFixtureCase{d.id, d.generatedClass, d.javaSource,
                                              core->runnerBody, core->expectedStdout,
                                              d.backends, d.generator});
        }
        return cases;
    }();
    return manifest;
}

}   // namespace mtest_set

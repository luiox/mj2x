#pragma once

#include <string>
#include <vector>

#include "gen/arith_samples.hpp"
#include "gen/array_samples.hpp"
#include "gen/const_load_samples.hpp"
#include "gen/control_flow_samples.hpp"
#include "gen/static_value_samples.hpp"
#include "gen/wave1_samples.hpp"
#include "gen/wave2_samples.hpp"
#include "gen/wave3_samples.hpp"
#include "gen/wave4_samples.hpp"
#include "manifest/core_fixture_registry.hpp"

namespace mtest_set {

enum class SampleWave
{
    Baseline,
    Wave1,
    Wave2,
    Wave3,
    Wave4,
};

struct SemanticFixtureCase
{
    const char* id;
    SampleWave  wave;
    const char* mainClass;        // 来自 core registry
    const char* runnerBody;       // 来自 core registry
    const char* expectedStdout;   // 来自 core registry
    std::vector<int> opcodes;     // 来自 core registry
    SampleBundle (*generator)(ca::str::Utf8StringArena&);
    std::vector<const char*> semantics;
};

inline SampleBundle bundle(const char* path, std::vector<ca::u8> bytes)
{
    return singleArtifact(path, std::move(bytes));
}

/// semantic manifest 的领域描述：id + 波次 + generator + 语义标签。
/// mainClass / runnerBody / expectedStdout / opcodes 从 core registry join，不在此重述。
struct SemanticDescriptor
{
    const char* id;
    SampleWave  wave;
    SampleBundle (*generator)(ca::str::Utf8StringArena&);
    std::vector<const char*> semantics;
};

inline const std::vector<SemanticFixtureCase>& semanticFixtureManifest()
{
    static const std::vector<SemanticDescriptor> descriptors = {
        {"g1.const_ints", SampleWave::Baseline,
         [](auto& arena) { return bundle("ConstInts.class", const_load::genConstInts(arena)); }, {}},
        {"g1.ldc_string", SampleWave::Baseline,
         [](auto& arena) { return bundle("ConstLdcString.class", const_load::genLdcString(arena)); }, {}},
        {"g2.add", SampleWave::Baseline,
         [](auto& arena) { return bundle("ArithAdd.class", arith::genAdd(arena)); }, {}},
        {"g5.abs", SampleWave::Baseline,
         [](auto& arena) { return bundle("CfAbs.class", control_flow::genAbs(arena)); }, {}},
        {"g8.sum_array", SampleWave::Baseline,
         [](auto& arena) { return bundle("ArrSumArray.class", arrays::genSumArray(arena)); }, {}},
        {"g3.integral_narrowing", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("TypeIntegralNarrowing.class", wave1::genIntegralNarrowing(arena)); }, {}},
        {"g3.numeric_widening", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("TypeNumericWidening.class", wave1::genNumericWidening(arena)); }, {}},
        {"g3.numeric_narrowing", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("TypeNumericNarrowing.class", wave1::genNumericNarrowing(arena)); }, {}},
        {"g6.static_special", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("CallsStaticSpecial.class", wave1::genStaticSpecial(arena)); }, {}},
        {"g6.virtual_interface", SampleWave::Wave1,
         [](auto& arena) { return wave1::genVirtualInterface(arena); }, {}},
        {"g6.recursion", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("CallsRecursion.class", wave1::genRecursion(arena)); }, {}},
        {"g7.static_fields", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("FieldsStatic.class", wave1::genStaticFields(arena)); }, {}},
        {"g7.instance_fields", SampleWave::Wave1,
         [](auto& arena) { return wave1::single("FieldsInstance.class", wave1::genInstanceFields(arena)); }, {}},
        {"g9.object_lifecycle", SampleWave::Wave2,
         [](auto& arena) { return wave2::single("ObjectLifecycle.class", wave2::genObjectLifecycle(arena)); },
         {"object.lifecycle"}},
        {"g9.type_checks", SampleWave::Wave2,
         [](auto& arena) { return wave2::single("TypeChecks.class", wave2::genTypeChecks(arena)); },
         {"object.type-check"}},
        {"g10.throw_catch", SampleWave::Wave2,
         [](auto& arena) { return wave2::single("ExceptionFlow.class", wave2::genExceptionFlow(arena)); },
         {"exception.try-catch"}},
        {"g10.monitor", SampleWave::Wave2,
         [](auto& arena) { return wave2::single("MonitorFlow.class", wave2::genMonitorFlow(arena)); },
         {"monitor.exception-release"}},
        {"g11.reflection_access", SampleWave::Wave2,
         [](auto& arena) { return wave2::genReflectionAccess(arena); },
         {"reflection.class", "reflection.method", "reflection.field"}},
        // ---- G12-G15（Wave3，#820：吸收 bludnov/ttvar 语义覆盖面）----
        {"g12.string_ops", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("StringOps.class", wave3::genStringOps(arena)); },
         {"string.length", "string.charAt"}},
        {"g12.string_compare", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("StringCompare.class", wave3::genStringCompare(arena)); },
         {"string.equals", "string.compare-to", "string.ignore-case"}},
        {"g13.bit_ops", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("BitOps.class", wave3::genBitOps(arena)); },
         {"bit.shift", "bit.mask", "bit.xor"}},
        {"g13.shift_out_of_range", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("ShiftOutOfRange.class", wave3::genShiftOutOfRange(arena)); },
         {"bit.shift-mask", "bit.shift-out-of-range"}},
        {"g14.deep_recursion", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("DeepRecursion.class", wave3::genDeepRecursion(arena)); },
         {"call.recursion"}},
        {"g15.collection_ops", SampleWave::Wave3,
         [](auto& arena) { return wave3::single("CollectionOps.class", wave3::genCollectionOps(arena)); },
         {"call.interface"}},
        // ---- G16（Wave3，#960：static final 常量折叠靶样本）----
        {"g16.cv_constant", SampleWave::Wave3,
         [](auto& arena) { return bundle("SvfCvConstant.class", static_value::genCvConstant(arena)); },
         {"field.static-final-constant-value"}},
        {"g16.clinit_constant", SampleWave::Wave3,
         [](auto& arena) { return bundle("SvfClinitConstant.class", static_value::genClinitConstant(arena)); },
         {"field.static-final-clinit"}},
        // ---- G17（Wave4，#1117：jvmtp 1.7.1 正确性修复语义点回归样本）----
        {"g17.math_nan_zero", SampleWave::Wave4,
         [](auto& arena) { return bundle("MathNanZero.class", wave4::genMathNanZero(arena)); },
         {"math.max-min-nan", "math.signed-zero", "math.round-boundary"}},
        {"g17.string_intern", SampleWave::Wave4,
         [](auto& arena) { return bundle("StringIntern.class", wave4::genStringIntern(arena)); },
         {"string.ldc-identity", "string.intern"}},
        {"g17.array_store_check", SampleWave::Wave4,
         [](auto& arena) { return bundle("ArrayStoreCheck.class", wave4::genArrayStoreCheck(arena)); },
         {"array.covariant-store-ase"}},
        {"g17.sibling_methods", SampleWave::Wave4,
         [](auto& arena) { return bundle("SiblingMethods.class", wave4::genSiblingMethods(arena)); },
         {"method.sibling-integrity"}},
        {"g17.annotation_visibility", SampleWave::Wave4,
         [](auto& arena) { return wave4::genAnnotationProbe(arena); },
         {"annotation.visibility", "annotation.default-value"}},
    };

    static const std::vector<SemanticFixtureCase> manifest = [] {
        std::vector<SemanticFixtureCase> cases;
        cases.reserve(descriptors.size());
        for (const auto& d : descriptors) {
            const CoreFixture* core = &requireCoreFixture(d.id, "semantic");
            cases.push_back(SemanticFixtureCase{core->id, d.wave, core->mainClass,
                                                core->runnerBody, core->expectedStdout,
                                                core->opcodes, d.generator, d.semantics});
        }
        return cases;
    }();
    return manifest;
}

} // namespace mtest_set

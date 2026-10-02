/// indy_bootstrap_plan_test.cpp —— #1027 B3 单源收敛对拍。
///
/// invokedynamic bootstrap 解析三份平行实现（StackC stackc_mir_invoke.cpp /
/// RegC regc_backend.cpp / LLVM llvm_ir_helpers.cpp）收敛到
/// backend/indy_bootstrap_plan。本测试以切换前三份游走的公共行为为 golden：
///   - bsm 形参游走 ≥10 组样本（固定参/varargs/耗尽补 NULL/空形参/边界），
///     输出槽结构逐字段断言（kind/bsmArgIndex/varargCount/formalSort）；
///   - Handle tag→Lookup.find* 全 8 tag + 未知 tag；
///   - bsm 常量分类全类型 + 边界值（i32/i64 极值）；
///   - 装箱类名映射全原语 + L/[。
/// 发射文本面回归由既有后端测试承担（regc_backend_e2e / stackc_backend /
/// llvm_ir_backend / invokedynamic_preprocessor）。

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mj2x/backend/indy_bootstrap_plan.hpp"

namespace {

using namespace mj2x;
using Kind = IndyBsmSlotKind;

/// 经典 LMF：Lookup+name+type+Object[] varargs。
constexpr const char* kLmfDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "[Ljava/lang/Object;)I";
/// 固定参形态：Lookup+name+type+两个静态参数（无 varargs）。
constexpr const char* kFixedDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "II)Ljava/lang/invoke/CallSite;";

std::vector<IndyBsmSlot> plan(const char* desc, size_t args)
{
    return planIndyBsmSlots(desc, args);
}

TEST(IndyBootstrapPlanTest, WalkVarargsArityBoundaries)
{
    // varargs 形态 × bsmArgs 0..4：Varargs 槽 start/count 全边界。
    const struct
    {
        size_t argCount;
        size_t start;
        size_t count;
    } kCases[] = {
        // 经典 LMF：varargs 前无静态参数 → 起点序数恒 0；数量 = 静态实参总数
        //（bsmArgs 不含 Lookup/name/type 三件套，原式 size - ordinal）。
        {0, 0, 0},
        {1, 0, 1},
        {2, 0, 2},
        {3, 0, 3},
        {4, 0, 4},
    };
    for (const auto& c : kCases) {
        const auto slots = plan(kLmfDesc, c.argCount);
        ASSERT_EQ(slots.size(), 4u) << "args=" << c.argCount;
        EXPECT_EQ(slots[0].kind, Kind::Lookup);
        EXPECT_EQ(slots[1].kind, Kind::SiteName);
        EXPECT_EQ(slots[2].kind, Kind::SiteType);
        EXPECT_EQ(slots[3].kind, Kind::Varargs);
        EXPECT_EQ(slots[3].bsmArgIndex, c.start);
        EXPECT_EQ(slots[3].varargCount, c.count);
    }
}

TEST(IndyBootstrapPlanTest, WalkFixedArityAndNullFill)
{
    // 固定参形态，实参恰好匹配：两静态槽各消费一个。
    auto slots = plan(kFixedDesc, 2);
    ASSERT_EQ(slots.size(), 5u);
    EXPECT_EQ(slots[3].kind, Kind::StaticArg);
    EXPECT_EQ(slots[3].bsmArgIndex, 0u);
    EXPECT_EQ(slots[4].kind, Kind::StaticArg);
    EXPECT_EQ(slots[4].bsmArgIndex, 1u);

    // 实参不足：耗尽后 NULL 补位（切换前三分支同构）。
    slots = plan(kFixedDesc, 1);
    ASSERT_EQ(slots.size(), 5u);
    EXPECT_EQ(slots[3].kind, Kind::StaticArg);
    EXPECT_EQ(slots[3].bsmArgIndex, 0u);
    EXPECT_EQ(slots[4].kind, Kind::Null);

    // 实参为 0：两个静态槽全 NULL。
    slots = plan(kFixedDesc, 0);
    ASSERT_EQ(slots.size(), 5u);
    EXPECT_EQ(slots[3].kind, Kind::Null);
    EXPECT_EQ(slots[4].kind, Kind::Null);

    // 实参超供：varargs 缺失时多余参数不进任何槽（形参面决定槽结构）。
    slots = plan(kFixedDesc, 5);
    ASSERT_EQ(slots.size(), 5u);
    EXPECT_EQ(slots[3].kind, Kind::StaticArg);
    EXPECT_EQ(slots[4].kind, Kind::StaticArg);
}

TEST(IndyBootstrapPlanTest, WalkEdgeShapes)
{
    // 空描述符：与切换前三后端一致抛 Invalid descriptor（畸形 bootstrap 原失败面）。
    EXPECT_THROW(plan("", 3), std::invalid_argument);

    // 尾随非 Object 数组（[I）不构成 varargs：按静态参数槽消费，formalSort=ARRAY。
    auto slots = plan(
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;[I)V",
        1);
    ASSERT_EQ(slots.size(), 4u);
    EXPECT_EQ(slots[3].kind, Kind::StaticArg);
    EXPECT_EQ(slots[3].formalSort, static_cast<int>(jbytecode::core::Type::ARRAY));

    // Object[] 出现在非尾随位：不构成 varargs。
    slots = plan(
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;[Ljava/lang/Object;I)Ljava/lang/invoke/CallSite;",
        2);
    ASSERT_EQ(slots.size(), 5u);
    EXPECT_EQ(slots[3].kind, Kind::StaticArg);
    EXPECT_EQ(slots[3].formalSort, static_cast<int>(jbytecode::core::Type::ARRAY));
    EXPECT_EQ(slots[4].kind, Kind::StaticArg);
    EXPECT_EQ(slots[4].formalSort, static_cast<int>(jbytecode::core::Type::INT));

    // 形参只有 2 个（畸形短 bootstrap）：只产出 lookup+name，无 name/type 三件套。
    slots = plan("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;)V", 0);
    ASSERT_EQ(slots.size(), 2u);
    EXPECT_EQ(slots[0].kind, Kind::Lookup);
    EXPECT_EQ(slots[1].kind, Kind::SiteName);
}

TEST(IndyBootstrapPlanTest, HandleFindAllTags)
{
    using Shape = IndyHandleArgShape;
    const struct
    {
        int         tag;
        const char* name;
        const char* desc;
        Shape       shape;
        bool        emitName;
    } kCases[] = {
        {jbytecode::core::Opcodes::H_GETFIELD, "findGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_GETSTATIC, "findStaticGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_PUTFIELD, "findSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_PUTSTATIC, "findStaticSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_INVOKESTATIC, "findStatic",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {jbytecode::core::Opcodes::H_INVOKESPECIAL, "findSpecial",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameTypeOwner, true},
        {jbytecode::core::Opcodes::H_NEWINVOKESPECIAL, "findConstructor",
         "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::TypeOnly, false},
        {jbytecode::core::Opcodes::H_INVOKEVIRTUAL, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {jbytecode::core::Opcodes::H_INVOKEINTERFACE, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {999, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},   // 未知 tag → default 分支
    };
    for (const auto& c : kCases) {
        const IndyHandleFind f = indyHandleFindFor(c.tag);
        EXPECT_STREQ(f.findName, c.name) << "tag=" << c.tag;
        EXPECT_STREQ(f.findDesc, c.desc) << "tag=" << c.tag;
        EXPECT_EQ(f.argShape, c.shape) << "tag=" << c.tag;
        EXPECT_EQ(f.emitName, c.emitName) << "tag=" << c.tag;
    }
}

TEST(IndyBootstrapPlanTest, ClassifyBsmArgAllKinds)
{
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(std::string("s"))), IndyBsmArgKind::String);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(ca::str::Utf8StringRef::from_cstr("u"))),
              IndyBsmArgKind::String);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(static_cast<ca::i32>(-2147483647 - 1))),
              IndyBsmArgKind::Int);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(static_cast<ca::i32>(2147483647))),
              IndyBsmArgKind::Int);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(static_cast<ca::i64>(-9223372036854775807ll - 1))),
              IndyBsmArgKind::Long);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(static_cast<ca::i64>(9223372036854775807ll))),
              IndyBsmArgKind::Long);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(1.5f)), IndyBsmArgKind::Float);
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(2.5)), IndyBsmArgKind::Double);

    jbytecode::core::Type methodType = jbytecode::core::Type::getMethodType(
        ca::str::Utf8StringRef::from_cstr("(II)I"));
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(methodType)), IndyBsmArgKind::MethodType);
    jbytecode::core::Type objectType = jbytecode::core::Type::getObjectType(
        ca::str::Utf8StringRef::from_cstr("java/lang/String"));
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(objectType)), IndyBsmArgKind::Class);

    jbytecode::core::Handle handle(jbytecode::core::Opcodes::H_INVOKESTATIC,
                                   ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                                   ca::str::Utf8StringRef::from_cstr("x"),
                                   ca::str::Utf8StringRef::from_cstr("(II)I"));
    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any(handle)), IndyBsmArgKind::Handle);

    EXPECT_EQ(classifyIndyBsmArg(ca::core::Any()), IndyBsmArgKind::Null);
}

TEST(IndyBootstrapPlanTest, BoxedClassNameAllPrimitives)
{
    EXPECT_STREQ(indyBoxedClassNameForDescHead('Z'), "java/lang/Boolean");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('B'), "java/lang/Byte");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('C'), "java/lang/Character");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('S'), "java/lang/Short");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('I'), "java/lang/Integer");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('J'), "java/lang/Long");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('F'), "java/lang/Float");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('D'), "java/lang/Double");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('L'), "java/lang/Object");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('['), "java/lang/Object");
    EXPECT_STREQ(indyBoxedClassNameForDescHead('V'), "java/lang/Object");
}

// ---------------------------------------------------------------------------
// 三方对拍：切换前三份平行实现逐字转录为参考（对拍对象 = 切换前代码路径），
// 与单源 plan 逐字段对齐。参考实现只在本测试内存活（与 A5 oldcore 同纪律）。
// ---------------------------------------------------------------------------

using Slots = std::vector<IndyBsmSlot>;

/// StackC stackc_mir_invoke.cpp 原游走（emitInvokeDynamic 内循环逐字转录）。
Slots referenceStackCWalk(const std::string& bsmDesc, size_t bsmArgCount,
                          const std::vector<jbytecode::core::Type>& bsmArgTypes)
{
    Slots                     slots;
    size_t                    bsmArgIndex = 0;
    for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
        if (i == 0) { slots.push_back(IndyBsmSlot{Kind::Lookup, 0, 0, 0}); continue; }
        if (i == 1) { slots.push_back(IndyBsmSlot{Kind::SiteName, 0, 0, 0}); continue; }
        if (i == 2) { slots.push_back(IndyBsmSlot{Kind::SiteType, 0, 0, 0}); continue; }
        const auto& formal = bsmArgTypes[i];
        ca::str::Utf8StringArena scratchA;
        const bool  isTrailingObjectArray = i + 1 == bsmArgTypes.size() &&
                                           formal.getSort() == jbytecode::core::Type::ARRAY &&
                                           formal.getDescriptor(scratchA).to_std_string() ==
                                               "[Ljava/lang/Object;";
        if (isTrailingObjectArray) {
            const size_t remaining =
                bsmArgIndex < bsmArgCount ? bsmArgCount - bsmArgIndex : 0;
            IndyBsmSlot slot{};
            slot.kind        = Kind::Varargs;
            slot.bsmArgIndex = bsmArgIndex;
            slot.varargCount = remaining;
            slots.push_back(slot);
            bsmArgIndex = bsmArgCount;
            continue;
        }
        if (bsmArgIndex >= bsmArgCount) {
            slots.push_back(IndyBsmSlot{Kind::Null, 0, 0, 0});
            continue;
        }
        IndyBsmSlot slot{};
        slot.kind        = Kind::StaticArg;
        slot.bsmArgIndex = bsmArgIndex;
        slot.formalSort  = static_cast<int>(formal.getSort());
        slots.push_back(slot);
        ++bsmArgIndex;
    }
    return slots;
}

/// RegC regc_backend.cpp 原游走（emitIndyBootstrapFunction 内循环逐字转录）。
Slots referenceRegCWalk(const std::string& bsmDesc, size_t bsmArgCount,
                        const std::vector<jbytecode::core::Type>& bsmArgTypes)
{
    Slots  slots;
    size_t bsmArgIndex = 0;
    for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
        if (i == 0) { slots.push_back(IndyBsmSlot{Kind::Lookup, 0, 0, 0}); continue; }
        if (i == 1) { slots.push_back(IndyBsmSlot{Kind::SiteName, 0, 0, 0}); continue; }
        if (i == 2) { slots.push_back(IndyBsmSlot{Kind::SiteType, 0, 0, 0}); continue; }
        const auto& formal = bsmArgTypes[i];
        ca::str::Utf8StringArena scratchB;
        const bool  isTrailingObjectArray =
            i + 1 == bsmArgTypes.size() && formal.getSort() == jbytecode::core::Type::ARRAY &&
            formal.getDescriptor(scratchB).to_std_string() == "[Ljava/lang/Object;";
        if (isTrailingObjectArray) {
            const size_t remaining = bsmArgIndex < bsmArgCount ? bsmArgCount - bsmArgIndex : 0;
            IndyBsmSlot  slot{};
            slot.kind        = Kind::Varargs;
            slot.bsmArgIndex = bsmArgIndex;
            slot.varargCount = remaining;
            slots.push_back(slot);
            bsmArgIndex = bsmArgCount;
            continue;
        }
        if (bsmArgIndex >= bsmArgCount) {
            slots.push_back(IndyBsmSlot{Kind::Null, 0, 0, 0});
            continue;
        }
        IndyBsmSlot slot{};
        slot.kind        = Kind::StaticArg;
        slot.bsmArgIndex = bsmArgIndex;
        slot.formalSort  = static_cast<int>(formal.getSort());
        slots.push_back(slot);
        ++bsmArgIndex;
    }
    return slots;
}

/// LLVM llvm_ir_helpers.cpp 原游走（emitIndyBootstrapFunction 内循环逐字转录）。
Slots referenceLlvmWalk(size_t bsmArgCount,
                        const std::vector<jbytecode::core::Type>& bsmArgTypes)
{
    Slots  slots;
    size_t bsmArgIndex = 0;
    for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
        if (i == 0) { slots.push_back(IndyBsmSlot{Kind::Lookup, 0, 0, 0}); continue; }
        if (i == 1) { slots.push_back(IndyBsmSlot{Kind::SiteName, 0, 0, 0}); continue; }
        if (i == 2) { slots.push_back(IndyBsmSlot{Kind::SiteType, 0, 0, 0}); continue; }
        const auto& formal = bsmArgTypes[i];
        ca::str::Utf8StringArena scratchC;
        const bool  isTrailingObjectArray =
            i + 1 == bsmArgTypes.size() && formal.getSort() == jbytecode::core::Type::ARRAY &&
            formal.getDescriptor(scratchC).to_std_string() == "[Ljava/lang/Object;";
        if (isTrailingObjectArray) {
            const size_t remaining = bsmArgIndex < bsmArgCount ? bsmArgCount - bsmArgIndex : 0;
            IndyBsmSlot  slot{};
            slot.kind        = Kind::Varargs;
            slot.bsmArgIndex = bsmArgIndex;
            slot.varargCount = remaining;
            slots.push_back(slot);
            bsmArgIndex = bsmArgCount;
            continue;
        }
        if (bsmArgIndex >= bsmArgCount) {
            slots.push_back(IndyBsmSlot{Kind::Null, 0, 0, 0});
            continue;
        }
        IndyBsmSlot slot{};
        slot.kind        = Kind::StaticArg;
        slot.bsmArgIndex = bsmArgIndex;
        slot.formalSort  = static_cast<int>(formal.getSort());
        slots.push_back(slot);
        ++bsmArgIndex;
    }
    return slots;
}

void expectSameSlots(const Slots& expected, const Slots& actual, const char* tag)
{
    ASSERT_EQ(expected.size(), actual.size()) << tag;
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i].kind, actual[i].kind) << tag << " slot#" << i;
        EXPECT_EQ(expected[i].bsmArgIndex, actual[i].bsmArgIndex) << tag << " slot#" << i;
        EXPECT_EQ(expected[i].varargCount, actual[i].varargCount) << tag << " slot#" << i;
        EXPECT_EQ(expected[i].formalSort, actual[i].formalSort) << tag << " slot#" << i;
    }
}

TEST(IndyBootstrapPlanTest, ThreeWayWalkCrossCheck)
{
    // ≥13 组样本：经典 LMF（varargs 0..4）、固定参（耗尽/恰好/超供/零参）、
    // 静态参在 varargs 前（StringConcatFactory 形态）、尾随 [I、非尾随
    // Object[]、畸形短形参表、仅三件套。
    const struct
    {
        const char* desc;
        size_t      argCount;
    } kSamples[] = {
        {kLmfDesc, 0}, {kLmfDesc, 1}, {kLmfDesc, 2}, {kLmfDesc, 3}, {kLmfDesc, 4},
        {kFixedDesc, 0}, {kFixedDesc, 1}, {kFixedDesc, 2}, {kFixedDesc, 5},
        {"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
         "MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;",
         2},
        {"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
         "MethodType;[I)V",
         1},
        {"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
         "MethodType;[Ljava/lang/Object;I)V",
         2},
        {"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;)V", 0},
        {"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
         "MethodType;)V",
         0},
    };
    for (const auto& sample : kSamples) {
        const auto bsmArgTypes = jbytecode::core::Type::getArgumentTypes(
            ca::str::Utf8StringRef::from_cstr(sample.desc));
        const auto mine = planIndyBsmSlots(sample.desc, sample.argCount);
        SCOPED_TRACE(sample.desc);
        expectSameSlots(
            referenceStackCWalk(sample.desc, sample.argCount, bsmArgTypes), mine, "StackC");
        expectSameSlots(
            referenceRegCWalk(sample.desc, sample.argCount, bsmArgTypes), mine, "RegC");
        expectSameSlots(
            referenceLlvmWalk(sample.argCount, bsmArgTypes), mine, "LLVM");
    }
}

TEST(IndyBootstrapPlanTest, ThreeWayHandleFindCrossCheck)
{
    // 切换前 StackC/RegC 两份同式 switch 转录为参考表，与单源逐字段对拍。
    using Shape = IndyHandleArgShape;
    const struct
    {
        int         tag;
        const char* name;
        const char* desc;
        Shape       shape;
        bool        emitName;
    } kReference[] = {
        {jbytecode::core::Opcodes::H_GETFIELD, "findGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_GETSTATIC, "findStaticGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_PUTFIELD, "findSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_PUTSTATIC, "findStaticSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameFieldType, true},
        {jbytecode::core::Opcodes::H_INVOKESTATIC, "findStatic",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {jbytecode::core::Opcodes::H_INVOKESPECIAL, "findSpecial",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameTypeOwner, true},
        {jbytecode::core::Opcodes::H_NEWINVOKESPECIAL, "findConstructor",
         "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::TypeOnly, false},
        {jbytecode::core::Opcodes::H_INVOKEVIRTUAL, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {jbytecode::core::Opcodes::H_INVOKEINTERFACE, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
        {999, "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;",
         Shape::NameType, true},
    };
    for (const auto& ref : kReference) {
        const auto find = indyHandleFindFor(ref.tag);
        EXPECT_STREQ(find.findName, ref.name) << "tag=" << ref.tag;
        EXPECT_STREQ(find.findDesc, ref.desc) << "tag=" << ref.tag;
        EXPECT_EQ(find.argShape, ref.shape) << "tag=" << ref.tag;
        EXPECT_EQ(find.emitName, ref.emitName) << "tag=" << ref.tag;
    }
}

/// RegC emitIndyBsmArg 原 if 链判定序转录（String → Utf8StringRef → Type →
/// Handle → i32 → i64 → float → double → Null）。
IndyBsmArgKind classifyRegCRef(const ca::core::Any& value)
{
    if (value.as<std::string>() != nullptr) return IndyBsmArgKind::String;
    if (value.as<ca::str::Utf8StringRef>() != nullptr) return IndyBsmArgKind::String;
    if (const auto* type = value.as<jbytecode::core::Type>()) {
        return type->getSort() == jbytecode::core::Type::METHOD ? IndyBsmArgKind::MethodType
                                                                : IndyBsmArgKind::Class;
    }
    if (value.as<jbytecode::core::Handle>() != nullptr) return IndyBsmArgKind::Handle;
    if (value.as<ca::i32>() != nullptr) return IndyBsmArgKind::Int;
    if (value.as<ca::i64>() != nullptr) return IndyBsmArgKind::Long;
    if (value.as<float>() != nullptr) return IndyBsmArgKind::Float;
    if (value.as<double>() != nullptr) return IndyBsmArgKind::Double;
    return IndyBsmArgKind::Null;
}

TEST(IndyBootstrapPlanTest, ThreeWayClassifyCrossCheck)
{
    jbytecode::core::Type methodType = jbytecode::core::Type::getMethodType(
        ca::str::Utf8StringRef::from_cstr("(II)I"));
    jbytecode::core::Type objectType = jbytecode::core::Type::getObjectType(
        ca::str::Utf8StringRef::from_cstr("java/lang/String"));
    jbytecode::core::Type intType =
        jbytecode::core::Type::getObjectType(ca::str::Utf8StringRef::from_cstr("I"));
    jbytecode::core::Handle handle(jbytecode::core::Opcodes::H_INVOKESTATIC,
                                   ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                                   ca::str::Utf8StringRef::from_cstr("x"),
                                   ca::str::Utf8StringRef::from_cstr("(II)I"));
    const std::pair<ca::core::Any, IndyBsmArgKind> kSamples[] = {
        {ca::core::Any(std::string("s")), IndyBsmArgKind::String},
        {ca::core::Any(ca::str::Utf8StringRef::from_cstr("s")), IndyBsmArgKind::String},
        {ca::core::Any(methodType), IndyBsmArgKind::MethodType},
        {ca::core::Any(objectType), IndyBsmArgKind::Class},
        {ca::core::Any(intType), IndyBsmArgKind::Class},
        {ca::core::Any(handle), IndyBsmArgKind::Handle},
        {ca::core::Any(static_cast<ca::i32>(-2147483647 - 1)), IndyBsmArgKind::Int},
        {ca::core::Any(static_cast<ca::i32>(2147483647)), IndyBsmArgKind::Int},
        {ca::core::Any(static_cast<ca::i64>(-9223372036854775807ll - 1)), IndyBsmArgKind::Long},
        {ca::core::Any(static_cast<ca::i64>(9223372036854775807ll)), IndyBsmArgKind::Long},
        {ca::core::Any(1.5f), IndyBsmArgKind::Float},
        {ca::core::Any(2.5), IndyBsmArgKind::Double},
        {ca::core::Any(), IndyBsmArgKind::Null},
    };
    for (const auto& sample : kSamples) {
        EXPECT_EQ(classifyIndyBsmArg(sample.first), classifyRegCRef(sample.first));
        EXPECT_EQ(classifyIndyBsmArg(sample.first), sample.second);
    }
}

TEST(IndyBootstrapPlanTest, ThreeWayBoxedNameCrossCheck)
{
    // RegC indyBoxedClassName 原实现逐值转录（含空串与默认分支）。
    const struct
    {
        const char* desc;
        const char* boxed;
    } kCases[] = {
        {"Z", "java/lang/Boolean"},   {"B", "java/lang/Byte"},
        {"C", "java/lang/Character"}, {"S", "java/lang/Short"},
        {"I", "java/lang/Integer"},   {"J", "java/lang/Long"},
        {"F", "java/lang/Float"},     {"D", "java/lang/Double"},
        {"", "java/lang/Object"},     {"V", "java/lang/Object"},
        {"Ljava/lang/String;", "java/lang/Object"},
        {"[I", "java/lang/Object"},
    };
    for (const auto& c : kCases) {
        const std::string s(c.desc);
        EXPECT_STREQ(indyBoxedClassNameForDescHead(s.empty() ? '\0' : s[0]), c.boxed)
            << "desc=" << c.desc;
    }
}

}   // namespace
